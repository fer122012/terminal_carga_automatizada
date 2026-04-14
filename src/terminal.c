#include "../include/terminal.h"

/* ── Inicialización del contexto global ── */
void terminal_init(ContextoTerminal *ctx,
                   AlgoritmoPlanificacion alg,
                   int num_camiones)
{
    memset(ctx, 0, sizeof(ContextoTerminal));
    ctx->algoritmo    = alg;
    ctx->num_camiones = num_camiones;

    /* Semáforo: NUM_MUELLES slots disponibles */
    sem_init(&ctx->sem_muelles, 0, NUM_MUELLES);

    /* Mutex para log y para cambios de estado */
    pthread_mutex_init(&ctx->mutex_log,    NULL);
    pthread_mutex_init(&ctx->mutex_estado, NULL);

    /* Cola de planificación */
    cola_init(&ctx->cola);

    /* Archivo de log */
    log_init(ctx);

    /* Generar camiones con datos aleatorios */
    srand((unsigned)time(NULL));
    for (int i = 0; i < num_camiones; i++) {
        ctx->camiones[i].id            = i;
        ctx->camiones[i].tipo          = (rand() % 3 == 0)
                                         ? CARGA_PERECEDERA
                                         : CARGA_NORMAL;
        ctx->camiones[i].burst         = (rand() % 5) + 1; /* 1-5 seg */
        ctx->camiones[i].burst_restante = ctx->camiones[i].burst;
        ctx->camiones[i].prioridad     = ctx->camiones[i].tipo;
        ctx->camiones[i].estado        = ESTADO_NUEVO;
    }

    printf("\n=== TERMINAL DE CARGA INICIADA ===\n");
    printf("Muelles disponibles : %d\n", NUM_MUELLES);
    printf("Camiones a procesar : %d\n", num_camiones);
    printf("Algoritmo           : %s\n\n",
           (alg == ALGORITMO_FIFO) ? "FIFO" : "Round Robin");
}

/* ── Lanzar todos los hilos y esperar su terminación ── */
void terminal_run(ContextoTerminal *ctx) {
    pthread_t hilos[MAX_CAMIONES];

    /* Crear hilos */
    for (int i = 0; i < ctx->num_camiones; i++) {
        /* Pasamos contexto + id mediante una estructura en heap */
        typedef struct { ContextoTerminal *ctx; int id; } Arg;
        Arg *a = malloc(sizeof(Arg));
        a->ctx = ctx;
        a->id  = i;
        if (pthread_create(&hilos[i], NULL, hilo_camion, a) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    /* Esperar a que todos terminen */
    for (int i = 0; i < ctx->num_camiones; i++) {
        pthread_join(hilos[i], NULL);
    }

    printf("\n=== TODOS LOS CAMIONES PROCESADOS ===\n\n");
}

/* ── Imprimir tabla comparativa de métricas ── */
void terminal_imprimir_resumen(ContextoTerminal *ctx) {
    printf("%-10s %-10s %-8s %-12s %-12s %-10s\n",
           "Camion", "Tipo", "Burst", "T.Espera", "T.Retorno", "Estado");
    printf("---------------------------------------------------------------\n");

    double sum_espera = 0, sum_retorno = 0;

    for (int i = 0; i < ctx->num_camiones; i++) {
        Camion *c = &ctx->camiones[i];
        printf("Camion-%02d  %-10s %-8d %-12.2f %-12.2f %-10s\n",
               c->id,
               (c->tipo == CARGA_PERECEDERA) ? "Perecedera" : "Normal",
               c->burst,
               c->t_espera,
               c->t_retorno,
               estado_str(c->estado));
        sum_espera  += c->t_espera;
        sum_retorno += c->t_retorno;
    }

    printf("---------------------------------------------------------------\n");
    printf("Promedio T.Espera  : %.2f seg\n",
           sum_espera  / ctx->num_camiones);
    printf("Promedio T.Retorno : %.2f seg\n\n",
           sum_retorno / ctx->num_camiones);
}

/* ── Liberar recursos ── */
void terminal_destroy(ContextoTerminal *ctx) {
    sem_destroy(&ctx->sem_muelles);
    pthread_mutex_destroy(&ctx->mutex_log);
    pthread_mutex_destroy(&ctx->mutex_estado);
    pthread_mutex_destroy(&ctx->cola.mutex_cola);
    log_cerrar(ctx);
}
