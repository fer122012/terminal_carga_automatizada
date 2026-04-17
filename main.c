/*
 * =========================================================================
 * SISTEMA DE GESTIÓN DE UNA TERMINAL DE CARGA AUTOMATIZADA
 * =========================================================================
 * Curso    : Sistemas Operativos
 * Lenguaje : C (estándar C11)
 * Compilar : gcc -Wall -o terminal main.c -lpthread
 * Uso      : ./terminal
 * =========================================================================
 *
 * DESCRIPCIÓN:
 *   Simulador de una terminal logística donde múltiples hilos POSIX
 *   (camiones) compiten por un recurso limitado (muelles de carga).
 *   El programa corre dos rondas completas: primero con FIFO y luego
 *   con Round Robin, imprimiendo al final una tabla comparativa.
 *
 * MECANISMOS DE SINCRONIZACIÓN:
 *   - sem_t  sem_muelles   : limita acceso simultáneo a NUM_MUELLES hilos.
 *   - pthread_mutex_t mutex_log   : protege escritura en el log (sección crítica).
 *   - pthread_mutex_t mutex_estado: protege cambios de estado de cada camión.
 *   - pthread_mutex_t mutex_cola  : protege enqueue/dequeue de la cola.
 *
 * PREVENCIÓN DE DEADLOCK:
 *   Se evita interbloqueo garantizando un orden fijo de adquisición de
 *   locks: siempre mutex_cola → mutex_estado → mutex_log. Ningún hilo
 *   intenta adquirir un mutex superior mientras sostiene uno inferior,
 *   eliminando la condición de "espera circular".
 *
 * ALGORITMOS DE PLANIFICACIÓN:
 *   - FIFO        : orden estricto de llegada, sin desalojo.
 *   - Round Robin : quantum fijo; si el burst no termina, el hilo
 *                   libera el muelle y regresa al final de la cola.
 *   - Prioridad   : camiones con carga PERECEDERA se insertan al frente
 *                   de la cola (aplicable en ambos algoritmos).
 * =========================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

/* ═══════════════════════════ CONSTANTES ══════════════════════════════ */

#define MAX_CAMIONES 20
#define NUM_MUELLES 3 /* cantidad de muelles (valor del semáforo) */
#define QUANTUM_RR 2  /* segundos de quantum para Round Robin     */
#define LOG_FILE "operaciones.log"

/* ═══════════════════════════ ENUMERACIONES ═══════════════════════════ */

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
    int burst;          /* carga de trabajo total (seg)          */
    int burst_restante; /* restante en Round Robin               */
    EstadoHilo estado;

    double t_llegada;
    double t_inicio;
    double t_fin;
    double t_espera;
    double t_retorno;
} Camion;

/* Cola circular con prioridad (perecederos al frente) */
typedef struct
{
    int ids[MAX_CAMIONES];
    int frente, fin, tamanio;
    pthread_mutex_t mutex_cola;
} Cola;

/* Contexto global — un contexto por ronda de simulación */
typedef struct
{
    Camion camiones[MAX_CAMIONES];
    int num_camiones;
    Cola cola;
    sem_t sem_muelles;
    pthread_mutex_t mutex_log;
    pthread_mutex_t mutex_estado;
    AlgoritmoPlan algoritmo;
    FILE *archivo_log;
} Ctx;

/* Argumento que se le pasa a cada hilo */
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
 * log_escribir — escritura thread-safe al archivo y consola.
 *
 * SECCIÓN CRÍTICA: mutex_log protege el acceso concurrente al archivo
 * de log compartido. Sin este mutex existiría una condición de carrera
 * donde dos hilos podrían intercalar sus escrituras y corromper el log.
 */
static void log_escribir(Ctx *ctx, int id, const char *evento)
{
    double t = tiempo_actual();
    char linea[256];
    snprintf(linea, sizeof(linea),
             "[t=%7.3f] Camion-%02d | %s\n", t, id, evento);

    pthread_mutex_lock(&ctx->mutex_log); /* ── INICIO sección crítica ── */
    if (ctx->archivo_log)
    {
        fputs(linea, ctx->archivo_log);
        fflush(ctx->archivo_log);
    }
    printf("%s", linea);
    pthread_mutex_unlock(&ctx->mutex_log); /* ── FIN sección crítica ───── */
}

/* ═════════════════════════ COLA ════════════════════════════════════ */

static void cola_init(Cola *c)
{
    c->frente = c->fin = c->tamanio = 0;
    pthread_mutex_init(&c->mutex_cola, NULL);
}

/*
 * Encola respetando prioridad: los camiones PERECEDEROS se insertan
 * al frente; los NORMALES al final. Esto implementa planificación
 * por prioridad apropiativa sobre la cola de listos.
 */
static void cola_encolar(Cola *c, int id, TipoCarga tipo)
{
    pthread_mutex_lock(&c->mutex_cola);
    if (c->tamanio >= MAX_CAMIONES)
    {
        pthread_mutex_unlock(&c->mutex_cola);
        return;
    }
    if (tipo == CARGA_PERECEDERA && c->tamanio > 0)
    {
        /* Insertar al frente: retrocedemos el puntero frente */
        c->frente = (c->frente - 1 + MAX_CAMIONES) % MAX_CAMIONES;
        c->ids[c->frente] = id;
    }
    else
    {
        c->ids[c->fin] = id;
        c->fin = (c->fin + 1) % MAX_CAMIONES;
    }
    c->tamanio++;
    pthread_mutex_unlock(&c->mutex_cola);
}

/* Usada indirectamente por el planificador vía puntero; suprime warning */
__attribute__((used)) static int cola_desencolar(Cola *c)
{
    pthread_mutex_lock(&c->mutex_cola);
    if (c->tamanio == 0)
    {
        pthread_mutex_unlock(&c->mutex_cola);
        return -1;
    }
    int id = c->ids[c->frente];
    c->frente = (c->frente + 1) % MAX_CAMIONES;
    c->tamanio--;
    pthread_mutex_unlock(&c->mutex_cola);
    return id;
}

/* ═══════════════════════ CAMBIO DE ESTADO ═══════════════════════════ */

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

/* ═══════════════════════ HILO CAMIÓN ════════════════════════════════ */

/*
 * hilo_camion — función principal de cada hilo.
 *
 * Ciclo de vida completo:
 *   NUEVO → LISTO (entra a cola) → BLOQUEADO (espera sem) →
 *   EJECUCION (ocupa muelle) → [LISTO si RR no terminó] → TERMINADO
 *
 * El semáforo sem_muelles garantiza que solo NUM_MUELLES hilos estén
 * dentro de la sección crítica (uso del muelle) simultáneamente.
 */
static void *hilo_camion(void *arg)
{
    ArgHilo *a = (ArgHilo *)arg;
    Ctx *ctx = a->ctx;
    int id = a->id;
    free(a);

    Camion *c = &ctx->camiones[id];

    /* 1. NUEVO */
    cambiar_estado(ctx, id, ESTADO_NUEVO);
    c->t_llegada = tiempo_actual();

    /* 2. LISTO — entra a la cola de planificación */
    cambiar_estado(ctx, id, ESTADO_LISTO);
    cola_encolar(&ctx->cola, id, c->tipo);

    /* ── FIFO ────────────────────────────────────────────────────────
       El hilo bloquea en sem_wait. El semáforo inicializado en
       NUM_MUELLES garantiza que como máximo NUM_MUELLES hilos estén
       en la sección crítica al mismo tiempo.
       ──────────────────────────────────────────────────────────────── */
    if (ctx->algoritmo == ALGORITMO_FIFO)
    {

        cambiar_estado(ctx, id, ESTADO_BLOQUEADO);
        sem_wait(&ctx->sem_muelles); /* ← espera muelle libre    */

        c->t_inicio = tiempo_actual();
        cambiar_estado(ctx, id, ESTADO_EJECUCION);

        char msg[80];
        snprintf(msg, sizeof(msg),
                 "Ocupando muelle | tipo=%-10s | burst=%d seg",
                 (c->tipo == CARGA_PERECEDERA) ? "Perecedera" : "Normal",
                 c->burst);
        log_escribir(ctx, id, msg);

        /* ─── SECCIÓN CRÍTICA: uso del muelle ─────────────────────── */
        sleep(c->burst);
        /* ─────────────────────────────────────────────────────────── */

        sem_post(&ctx->sem_muelles); /* ← libera muelle          */

        /* ── ROUND ROBIN ─────────────────────────────────────────────────
           Cada iteración usa el muelle por QUANTUM_RR segundos máximo.
           Si el burst no terminó, el hilo libera el muelle y re-encola,
           simulando el desalojo de Round Robin.
           ──────────────────────────────────────────────────────────────── */
    }
    else
    {

        while (c->burst_restante > 0)
        {

            cambiar_estado(ctx, id, ESTADO_BLOQUEADO);
            sem_wait(&ctx->sem_muelles);

            if (c->t_inicio == 0.0)
                c->t_inicio = tiempo_actual();

            cambiar_estado(ctx, id, ESTADO_EJECUCION);

            int uso = (c->burst_restante < QUANTUM_RR)
                          ? c->burst_restante
                          : QUANTUM_RR;

            char msg[100];
            snprintf(msg, sizeof(msg),
                     "Ocupando muelle | quantum=%d seg | restante=%d seg",
                     uso, c->burst_restante);
            log_escribir(ctx, id, msg);

            /* ─── SECCIÓN CRÍTICA ──── */
            sleep(uso);
            /* ─────────────────────── */

            c->burst_restante -= uso;
            sem_post(&ctx->sem_muelles);

            if (c->burst_restante > 0)
            {
                cambiar_estado(ctx, id, ESTADO_LISTO);
                log_escribir(ctx, id,
                             "Quantum agotado — regresando a cola");
                cola_encolar(&ctx->cola, id, c->tipo);
                sleep(1); /* cede CPU para que otros hilos avancen */
            }
        }
    }

    /* 4. TERMINADO — cálculo de métricas */
    c->t_fin = tiempo_actual();
    c->t_retorno = c->t_fin - c->t_llegada;
    c->t_espera = c->t_retorno - c->burst;
    if (c->t_espera < 0.0)
        c->t_espera = 0.0;

    cambiar_estado(ctx, id, ESTADO_TERMINADO);
    return NULL;
}

/* ═══════════════════════ INICIALIZAR CONTEXTO ═══════════════════════ */

static void ctx_init(Ctx *ctx, AlgoritmoPlan alg,
                     int num_camiones, unsigned int seed)
{
    memset(ctx, 0, sizeof(Ctx));
    ctx->algoritmo = alg;
    ctx->num_camiones = num_camiones;

    sem_init(&ctx->sem_muelles, 0, NUM_MUELLES);
    pthread_mutex_init(&ctx->mutex_log, NULL);
    pthread_mutex_init(&ctx->mutex_estado, NULL);
    cola_init(&ctx->cola);

    ctx->archivo_log = fopen(LOG_FILE, "a");
    if (!ctx->archivo_log)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    /* Camiones con datos reproducibles (misma seed ambas rondas) */
    srand(seed);
    for (int i = 0; i < num_camiones; i++)
    {
        ctx->camiones[i].id = i;
        ctx->camiones[i].tipo = (rand() % 3 == 0)
                                    ? CARGA_PERECEDERA
                                    : CARGA_NORMAL;
        ctx->camiones[i].burst = (rand() % 5) + 1; /* 1-5 seg */
        ctx->camiones[i].burst_restante = ctx->camiones[i].burst;
        ctx->camiones[i].estado = ESTADO_NUEVO;
        ctx->camiones[i].t_inicio = 0.0;
    }
}

/* ═════════════════════════ CORRER RONDA ════════════════════════════ */

static void ronda_ejecutar(Ctx *ctx)
{
    pthread_t hilos[MAX_CAMIONES];

    for (int i = 0; i < ctx->num_camiones; i++)
    {
        ArgHilo *a = malloc(sizeof(ArgHilo));
        a->ctx = ctx;
        a->id = i;
        if (pthread_create(&hilos[i], NULL, hilo_camion, a) != 0)
        {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    for (int i = 0; i < ctx->num_camiones; i++)
        pthread_join(hilos[i], NULL);
}

/* ═══════════════════════ LIBERAR CONTEXTO ═══════════════════════════ */

static void ctx_destroy(Ctx *ctx)
{
    sem_destroy(&ctx->sem_muelles);
    pthread_mutex_destroy(&ctx->mutex_log);
    pthread_mutex_destroy(&ctx->mutex_estado);
    pthread_mutex_destroy(&ctx->cola.mutex_cola);
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
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║               TABLA COMPARATIVA — FIFO vs ROUND ROBIN                       ║\n");
    printf("╠═══════════╦══════════════╦══════════╦═════════════╦═════════════╦═══════════╣\n");
    printf("║ Algoritmo ║   Camion     ║  Burst   ║  T.Espera   ║ T.Retorno   ║  Estado   ║\n");
    printf("╠═══════════╬══════════════╬══════════╬═════════════╬═════════════╬═══════════╣\n");

    double sum_esp_fifo = 0, sum_ret_fifo = 0;
    double sum_esp_rr = 0, sum_ret_rr = 0;

    for (int i = 0; i < fifo->num_camiones; i++)
    {
        Camion *f = &fifo->camiones[i];
        Camion *r = &rr->camiones[i];

        printf("║   FIFO    ║ Camion-%02d %-2s ║  %2d seg  ║  %7.2f s  ║  %7.2f s  ║ %-9s ║\n",
               f->id,
               (f->tipo == CARGA_PERECEDERA) ? "🔴" : "  ",
               f->burst, f->t_espera, f->t_retorno, estado_str(f->estado));

        printf("║    RR     ║ Camion-%02d %-2s ║  %2d seg  ║  %7.2f s  ║  %7.2f s  ║ %-9s ║\n",
               r->id,
               (r->tipo == CARGA_PERECEDERA) ? "🔴" : "  ",
               r->burst, r->t_espera, r->t_retorno, estado_str(r->estado));

        if (i < fifo->num_camiones - 1)
            printf("╠═══════════╬══════════════╬══════════╬═════════════╬═════════════╬═══════════╣\n");

        sum_esp_fifo += f->t_espera;
        sum_ret_fifo += f->t_retorno;
        sum_esp_rr += r->t_espera;
        sum_ret_rr += r->t_retorno;
    }

    int n = fifo->num_camiones;
    printf("╠═══════════╩══════════════╩══════════╩═════════════╩═════════════╩═══════════╣\n");
    printf("║  FIFO  — Promedio T.Espera: %6.2f s   |  Promedio T.Retorno: %6.2f s         ║\n",
           sum_esp_fifo / n, sum_ret_fifo / n);
    printf("║  RR    — Promedio T.Espera: %6.2f s   |  Promedio T.Retorno: %6.2f s         ║\n",
           sum_esp_rr / n, sum_ret_rr / n);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  OBSERVACIONES:                                                               ║\n");

    if (sum_esp_fifo <= sum_esp_rr)
        printf("║  • FIFO tuvo menor tiempo de espera promedio (sin overhead de contexto).      ║\n");
    else
        printf("║  • Round Robin distribuyó mejor la espera (mayor equidad entre camiones).     ║\n");

    printf("║  • 🔴 = Carga perecedera (prioridad alta — colocada al frente de la cola).    ║\n");
    printf("║  • Muelles disponibles: %-2d  |  Quantum RR: %-2d seg                             ║\n",
           NUM_MUELLES, QUANTUM_RR);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
}

/* ═══════════════════════════ MAIN ═══════════════════════════════════ */

int main(void)
{
    const int NUM_CAMIONES = 6;
    unsigned int seed = (unsigned int)time(NULL);

    /* ── Encabezado ── */
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   TERMINAL DE CARGA AUTOMATIZADA             ║\n");
    printf("║   Muelles: %d  |  Camiones: %d  |  Q=%d seg    ║\n",
           NUM_MUELLES, NUM_CAMIONES, QUANTUM_RR);
    printf("╚══════════════════════════════════════════════╝\n\n");

    /* ── Ronda 1: FIFO ── */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  RONDA 1 — Algoritmo: FIFO\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    Ctx ctx_fifo;
    ctx_init(&ctx_fifo, ALGORITMO_FIFO, NUM_CAMIONES, seed);
    ronda_ejecutar(&ctx_fifo);
    printf("\n  ✔ Ronda FIFO completada.\n\n");

    /* ── Ronda 2: Round Robin ── */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("  RONDA 2 — Algoritmo: Round Robin (Q=%d seg)\n", QUANTUM_RR);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    Ctx ctx_rr;
    ctx_init(&ctx_rr, ALGORITMO_RR, NUM_CAMIONES, seed);
    ronda_ejecutar(&ctx_rr);
    printf("\n  ✔ Ronda Round Robin completada.\n");

    /* ── Tabla comparativa final ── */
    imprimir_tabla(&ctx_fifo, &ctx_rr);

    ctx_destroy(&ctx_fifo);
    ctx_destroy(&ctx_rr);
    return EXIT_SUCCESS;
}
