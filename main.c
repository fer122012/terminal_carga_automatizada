#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>

/* ═══════════════════════════ CONSTANTES ═════════════════════════════ */

#define MAX_CAMIONES 20
#define NUM_MUELLES 3
#define QUANTUM_RR 2
#define LOG_FILE "operaciones.log"

/* ═══════════════════════════ ENUMERACIONES ═════════════════════════ */

typedef enum
{
    ESTADO_NUEVO,
    ESTADO_LISTO,
    ESTADO_EJECUCION,
    ESTADO_BLOQUEADO,
    ESTADO_TERMINADO
} EstadoHilo;

typedef enum
{
    ALGORITMO_FIFO,
    ALGORITMO_RR
} AlgoritmoPlan;

typedef enum
{
    CARGA_NORMAL = 1,
    CARGA_PERECEDERA = 0
} TipoCarga;

/* ═══════════════════════════ ESTRUCTURAS ════════════════════════════ */

typedef struct
{
    int id;
    TipoCarga tipo;
    int burst;
    int burst_restante;
    EstadoHilo estado;
    double t_llegada;
    double t_inicio;
    double t_fin;
    double t_espera;
    double t_retorno;
} Camion;

typedef struct
{
    int ids[MAX_CAMIONES];
    int frente, fin, tamanio;
} Cola;

typedef struct
{
    Camion camiones[MAX_CAMIONES];
    int num_camiones;
    int terminados;

    Cola cola;

    /*
     * sem_muelles: semáforo POSIX inicializado en NUM_MUELLES.
     */
    sem_t sem_muelles;

    pthread_mutex_t mutex_plan;
    pthread_cond_t cond_cola;                /* hay camiones en cola      */
    pthread_cond_t cond_listo[MAX_CAMIONES]; /* señal individual por hilo */
    int turno[MAX_CAMIONES];

    pthread_mutex_t mutex_estado;
    pthread_mutex_t mutex_log;

    AlgoritmoPlan algoritmo;
    FILE *archivo_log;
    double t_base;
} Ctx;

typedef struct
{
    Ctx *ctx;
    int id;
} ArgHilo;

/* ═════════════════════════ UTILIDADES ═══════════════════════════════ */

static double tiempo_actual(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *estado_str(EstadoHilo e)
{
    switch (e)
    {
    case ESTADO_NUEVO:
        return "NUEVO";
    case ESTADO_LISTO:
        return "LISTO";
    case ESTADO_EJECUCION:
        return "EJECUCION";
    case ESTADO_BLOQUEADO:
        return "BLOQUEADO";
    case ESTADO_TERMINADO:
        return "TERMINADO";
    default:
        return "?";
    }
}

/* ═════════════════════════ LOG (sección crítica) ════════════════════ */

/*
 * SECCIÓN CRÍTICA: mutex_log evita que dos hilos intercalen sus
 * escrituras en el archivo y corrompan las líneas (condición de carrera)
 */
static void log_escribir(Ctx *ctx, int id, const char *evento)
{
    double t = tiempo_actual() - ctx->t_base;
    char linea[256];
    snprintf(linea, sizeof(linea),
             "[t=%6.3f] Camion-%02d | %s\n", t, id, evento);

    /* ── INICIO sección crítica (mutex_log) ── */
    pthread_mutex_lock(&ctx->mutex_log);
    if (ctx->archivo_log)
    {
        fputs(linea, ctx->archivo_log);
        fflush(ctx->archivo_log);
    }
    printf("%s", linea);
    pthread_mutex_unlock(&ctx->mutex_log);
    /* ── FIN sección crítica ────────────────  */
}

/* ═══════════════════ COLA CON PRIORIDAD ESTABLE ════════════════════ */

static void cola_encolar(Ctx *ctx, int id)
{
    Cola *c = &ctx->cola;
    if (c->tamanio >= MAX_CAMIONES)
        return;

    TipoCarga tipo = ctx->camiones[id].tipo;

    if (tipo == CARGA_NORMAL || c->tamanio == 0)
    {
        c->ids[c->fin] = id;
        c->fin = (c->fin + 1) % MAX_CAMIONES;
    }
    else
    {
        /*
         * Perecedero
         */
        int n = c->tamanio;
        int pos = 0; 
        for (int i = 0; i < n; i++)
        {
            int idx = (c->frente + i) % MAX_CAMIONES;
            if (ctx->camiones[c->ids[idx]].tipo == CARGA_PERECEDERA)
                pos++;
            else
                break;
        }
        
        for (int i = n; i > pos; i--)
        {
            int dst = (c->frente + i) % MAX_CAMIONES;
            int src = (c->frente + i - 1) % MAX_CAMIONES;
            c->ids[dst] = c->ids[src];
        }
        c->ids[(c->frente + pos) % MAX_CAMIONES] = id;
        c->fin = (c->fin + 1) % MAX_CAMIONES;
    }
    c->tamanio++;
}

static int cola_desencolar(Ctx *ctx)
{
    Cola *c = &ctx->cola;
    if (c->tamanio == 0)
        return -1;
    int id = c->ids[c->frente];
    c->frente = (c->frente + 1) % MAX_CAMIONES;
    c->tamanio--;
    return id;
}

/* ════════════════════════ CAMBIO DE ESTADO ══════════════════════════ */

static void cambiar_estado(Ctx *ctx, int id, EstadoHilo nuevo)
{
    pthread_mutex_lock(&ctx->mutex_estado);
    EstadoHilo anterior = ctx->camiones[id].estado;
    ctx->camiones[id].estado = nuevo;
    pthread_mutex_unlock(&ctx->mutex_estado);

    if (anterior != nuevo)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Estado: %-9s -> %s",
                 estado_str(anterior), estado_str(nuevo));
        log_escribir(ctx, id, msg);
    }
}

/* ═══════════════════════ HILO PLANIFICADOR ══════════════════════════ */


static void *hilo_planificador(void *arg)
{
    Ctx *ctx = (Ctx *)arg;

    pthread_mutex_lock(&ctx->mutex_plan);
    while (ctx->terminados < ctx->num_camiones)
    {

        /* Espera mientras la cola esté vacía */
        while (ctx->cola.tamanio == 0 &&
               ctx->terminados < ctx->num_camiones)
            pthread_cond_wait(&ctx->cond_cola, &ctx->mutex_plan);

        if (ctx->terminados >= ctx->num_camiones)
            break;
        if (ctx->cola.tamanio == 0)
            continue;

        /* Elige al siguiente camión de la cola */
        int id = cola_desencolar(ctx);
        if (id < 0)
            continue;

        pthread_mutex_unlock(&ctx->mutex_plan);

        /* Marca BLOQUEADO al camión si el semáforo va a bloquear */
        int val;
        sem_getvalue(&ctx->sem_muelles, &val);
        if (val == 0)
            cambiar_estado(ctx, id, ESTADO_BLOQUEADO);

        sem_wait(&ctx->sem_muelles); /* reserva muelle  */

        pthread_mutex_lock(&ctx->mutex_plan);

        /* Despierta al camión elegido */
        ctx->turno[id] = 1;
        pthread_cond_signal(&ctx->cond_listo[id]);
    }
    pthread_mutex_unlock(&ctx->mutex_plan);
    return NULL;
}

/* ═══════════════════════════ HILO CAMIÓN ════════════════════════════ */

static void *hilo_camion(void *arg)
{
    ArgHilo *a = (ArgHilo *)arg;
    Ctx *ctx = a->ctx;
    int id = a->id;
    free(a);

    Camion *c = &ctx->camiones[id];

    /* 1. NUEVO: hilo creado */
    cambiar_estado(ctx, id, ESTADO_NUEVO);
    c->t_llegada = tiempo_actual() - ctx->t_base;

    /* 2. LISTO: cambia estado ANTES de encolarse */
    cambiar_estado(ctx, id, ESTADO_LISTO);
    pthread_mutex_lock(&ctx->mutex_plan);
    cola_encolar(ctx, id);
    pthread_cond_signal(&ctx->cond_cola);
    pthread_mutex_unlock(&ctx->mutex_plan);

    /* Ciclo principal */
    while (c->burst_restante > 0)
    {

        pthread_mutex_lock(&ctx->mutex_plan);
        while (!ctx->turno[id])
            pthread_cond_wait(&ctx->cond_listo[id], &ctx->mutex_plan);
        ctx->turno[id] = 0;
        pthread_mutex_unlock(&ctx->mutex_plan);

        /* 3. EJECUCION: muelle reservado por el planificador */
        if (c->t_inicio == 0.0)
            c->t_inicio = tiempo_actual() - ctx->t_base;
        cambiar_estado(ctx, id, ESTADO_EJECUCION);

        int uso = (ctx->algoritmo == ALGORITMO_FIFO)
                      ? c->burst_restante
                      : (c->burst_restante < QUANTUM_RR ? c->burst_restante : QUANTUM_RR);

        char msg[120];
        snprintf(msg, sizeof(msg),
                 "Ocupando muelle | tipo=%-10s | usando=%d seg | restante=%d seg",
                 (c->tipo == CARGA_PERECEDERA) ? "Perecedera" : "Normal",
                 uso, c->burst_restante);
        log_escribir(ctx, id, msg);

        /* ══ SECCIÓN CRÍTICA: trabajo de carga en el muelle ══ */
        sleep(uso);
        /* ═════════════════════════════════════════════════════ */

        c->burst_restante -= uso;

        /* Libera el muelle mediante sem_post  */
        sem_post(&ctx->sem_muelles);

        /* Round Robin: si no terminó, re-encola y vuelve a LISTO */
        if (c->burst_restante > 0)
        {
            log_escribir(ctx, id, "Quantum agotado — regresando a cola de listos");
            cambiar_estado(ctx, id, ESTADO_LISTO);
            pthread_mutex_lock(&ctx->mutex_plan);
            cola_encolar(ctx, id);
            pthread_cond_signal(&ctx->cond_cola);
            pthread_mutex_unlock(&ctx->mutex_plan);
        }
    }

    /* 4. TERMINADO */
    c->t_fin = tiempo_actual() - ctx->t_base;
    c->t_retorno = c->t_fin - c->t_llegada;
    c->t_espera = c->t_retorno - c->burst;
    if (c->t_espera < 0.0)
        c->t_espera = 0.0;
    cambiar_estado(ctx, id, ESTADO_TERMINADO);

    /* Avisa al planificador que terminó uno más */
    pthread_mutex_lock(&ctx->mutex_plan);
    ctx->terminados++;
    pthread_cond_signal(&ctx->cond_cola);
    pthread_mutex_unlock(&ctx->mutex_plan);

    return NULL;
}

/* ═══════════════════════ INICIALIZAR CONTEXTO ═══════════════════════ */

static void ctx_init(Ctx *ctx, AlgoritmoPlan alg,
                     int num_camiones, unsigned int seed)
{
    memset(ctx, 0, sizeof(Ctx));
    ctx->algoritmo = alg;
    ctx->num_camiones = num_camiones;
    ctx->terminados = 0;
    ctx->t_base = tiempo_actual();

    /* Semáforo POSIX inicializado en NUM_MUELLES */
    if (sem_init(&ctx->sem_muelles, 0, NUM_MUELLES) != 0)
    {
        perror("sem_init");
        exit(EXIT_FAILURE);
    }

    pthread_mutex_init(&ctx->mutex_plan, NULL);
    pthread_mutex_init(&ctx->mutex_estado, NULL);
    pthread_mutex_init(&ctx->mutex_log, NULL);
    pthread_cond_init(&ctx->cond_cola, NULL);
    for (int i = 0; i < num_camiones; i++)
    {
        pthread_cond_init(&ctx->cond_listo[i], NULL);
        ctx->turno[i] = 0;
    }

    ctx->cola.frente = ctx->cola.fin = ctx->cola.tamanio = 0;

    ctx->archivo_log = fopen(LOG_FILE, "a");
    if (!ctx->archivo_log)
    {
        perror("fopen log");
        exit(EXIT_FAILURE);
    }

    srand(seed);
    for (int i = 0; i < num_camiones; i++)
    {
        ctx->camiones[i].id = i;
        ctx->camiones[i].tipo = (rand() % 3 == 0)
                                    ? CARGA_PERECEDERA
                                    : CARGA_NORMAL;
        ctx->camiones[i].burst = (rand() % 5) + 1;
        ctx->camiones[i].burst_restante = ctx->camiones[i].burst;
        ctx->camiones[i].estado = ESTADO_NUEVO;
        ctx->camiones[i].t_inicio = 0.0;
    }
}

/* ═════════════════════════ CORRER RONDA ════════════════════════════ */

static void ronda_ejecutar(Ctx *ctx)
{
    pthread_t hilos[MAX_CAMIONES];
    pthread_t hilo_plan;
    int ret;

    ret = pthread_create(&hilo_plan, NULL, hilo_planificador, ctx);
    if (ret != 0)
    {
        fprintf(stderr, "Error al crear hilo planificador: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < ctx->num_camiones; i++)
    {
        ArgHilo *a = malloc(sizeof(ArgHilo));
        if (!a)
        {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        a->ctx = ctx;
        a->id = i;

        ret = pthread_create(&hilos[i], NULL, hilo_camion, a);
        if (ret != 0)
        {
            fprintf(stderr, "Error al crear Camion-%02d: %s\n", i, strerror(ret));
            free(a);
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < ctx->num_camiones; i++)
        pthread_join(hilos[i], NULL);
    pthread_join(hilo_plan, NULL);
}

/* ══════════════════════ LIBERAR CONTEXTO ════════════════════════════ */

static void ctx_destroy(Ctx *ctx)
{
    sem_destroy(&ctx->sem_muelles);
    pthread_mutex_destroy(&ctx->mutex_plan);
    pthread_mutex_destroy(&ctx->mutex_estado);
    pthread_mutex_destroy(&ctx->mutex_log);
    pthread_cond_destroy(&ctx->cond_cola);
    for (int i = 0; i < ctx->num_camiones; i++)
        pthread_cond_destroy(&ctx->cond_listo[i]);
    if (ctx->archivo_log)
    {
        fclose(ctx->archivo_log);
        ctx->archivo_log = NULL;
    }
}

/* ═══════════════════════ TABLA COMPARATIVA ══════════════════════════ */

static void imprimir_tabla(Ctx *fifo, Ctx *rr)
{
    printf("\n");
    printf("╔═════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║            TABLA COMPARATIVA — FIFO vs ROUND ROBIN                          ║\n");
    printf("╠═══════════╦══════════════╦══════════╦═════════════╦═════════════╦═══════════╣\n");
    printf("║ Algoritmo ║   Camion     ║  Burst   ║  T.Espera   ║ T.Retorno   ║  Estado   ║\n");
    printf("╠═══════════╬══════════════╬══════════╬═════════════╬═════════════╬═══════════╣\n");

    double sum_ef = 0, sum_rf = 0, sum_er = 0, sum_rr_t = 0;

    for (int i = 0; i < fifo->num_camiones; i++)
    {
        Camion *f = &fifo->camiones[i];
        Camion *r = &rr->camiones[i];
        const char *marca = (f->tipo == CARGA_PERECEDERA) ? "[P]" : "   ";

        printf("║   FIFO    ║ Camion-%02d%s ║  %2d seg  ║  %7.2f s  ║  %7.2f s  ║ %-9s ║\n",
               f->id, marca, f->burst, f->t_espera, f->t_retorno, estado_str(f->estado));
        printf("║    RR     ║ Camion-%02d%s ║  %2d seg  ║  %7.2f s  ║  %7.2f s  ║ %-9s ║\n",
               r->id, marca, r->burst, r->t_espera, r->t_retorno, estado_str(r->estado));

        if (i < fifo->num_camiones - 1)
            printf("╠═══════════╬══════════════╬══════════╬═════════════╬═════════════╬═══════════╣\n");

        sum_ef += f->t_espera;
        sum_rf += f->t_retorno;
        sum_er += r->t_espera;
        sum_rr_t += r->t_retorno;
    }

    int n = fifo->num_camiones;
    printf("╠═══════════╩══════════════╩══════════╩═════════════╩═════════════╩═══════════╣\n");
    printf("║  FIFO  — Prom. T.Espera: %6.2f s  |  Prom. T.Retorno: %6.2f s              ║\n",
           sum_ef / n, sum_rf / n);
    printf("║  RR    — Prom. T.Espera: %6.2f s  |  Prom. T.Retorno: %6.2f s              ║\n",
           sum_er / n, sum_rr_t / n);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  OBSERVACIONES                                                                ║\n");
    if (sum_ef <= sum_er)
        printf("║  • FIFO logro menor T.Espera promedio (sin overhead de cambio de contexto).  ║\n");
    else
        printf("║  • RR distribuyo mejor la espera (mayor equidad entre camiones).             ║\n");
    printf("║  • [P] = Carga perecedera (prioridad alta, FIFO estable entre iguales).      ║\n");
    printf("║  • Planificador llama sem_wait; camion entra directo (sin carrera).           ║\n");
    printf("║  • Muelles: %-2d  |  Quantum RR: %-2d seg  |  Sin deadlock: orden fijo locks.  ║\n",
           NUM_MUELLES, QUANTUM_RR);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
}

/* ═══════════════════════════ MAIN ═══════════════════════════════════ */

int main(void)
{
    const int NUM_CAMIONES = 6;
    unsigned int seed = (unsigned int)time(NULL);

    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   TERMINAL DE CARGA AUTOMATIZADA                 ║\n");
    printf("║   Muelles: %d  |  Camiones: %d  |  Quantum: %d seg  ║\n",
           NUM_MUELLES, NUM_CAMIONES, QUANTUM_RR);
    printf("║   Planificador: hilo dedicado con cola real       ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    Ctx ctx_fifo;
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  RONDA 1 — FIFO (orden estricto de llegada)\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    ctx_init(&ctx_fifo, ALGORITMO_FIFO, NUM_CAMIONES, seed);
    ronda_ejecutar(&ctx_fifo);
    printf("\n  Ronda FIFO completada.\n\n");

    Ctx ctx_rr;
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  RONDA 2 — Round Robin (quantum=%d seg)\n", QUANTUM_RR);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    ctx_init(&ctx_rr, ALGORITMO_RR, NUM_CAMIONES, seed);
    ronda_ejecutar(&ctx_rr);
    printf("\n  Ronda Round Robin completada.\n");

    imprimir_tabla(&ctx_fifo, &ctx_rr);

    ctx_destroy(&ctx_fifo);
    ctx_destroy(&ctx_rr);
    return EXIT_SUCCESS;
}
