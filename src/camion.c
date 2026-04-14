#include "../include/terminal.h"

/* Estructura auxiliar para pasar dos punteros al hilo */
typedef struct {
    ContextoTerminal *ctx;
    int               id;
} ArgHilo;

/* ──────────────────────────────────────────────────────────────────────
   Función principal del hilo (camión)
   Ciclo de vida: NUEVO → LISTO → (BLOQUEADO?) → EJECUCION → TERMINADO
   ────────────────────────────────────────────────────────────────────── */
void *hilo_camion(void *arg) {
    ArgHilo          *a   = (ArgHilo *)arg;
    ContextoTerminal *ctx = a->ctx;
    int               id  = a->id;
    free(a);

    Camion *c = &ctx->camiones[id];

    /* 1. NUEVO: hilo creado, registramos llegada */
    cambiar_estado(ctx, id, ESTADO_NUEVO);
    c->t_llegada = tiempo_actual();

    /* 2. LISTO: entra a la cola de planificación */
    cambiar_estado(ctx, id, ESTADO_LISTO);
    cola_encolar(&ctx->cola, id);

    /* ── FIFO ─────────────────────────────────────────────────────────
       El hilo bloquea en sem_wait hasta que haya muelle disponible.
       El semáforo garantiza que solo NUM_MUELLES hilos estén en
       sección crítica simultáneamente.
       ───────────────────────────────────────────────────────────────── */
    if (ctx->algoritmo == ALGORITMO_FIFO) {

        /* Posible BLOQUEADO si el semáforo está en 0 */
        cambiar_estado(ctx, id, ESTADO_BLOQUEADO);
        sem_wait(&ctx->sem_muelles);   /* ← espera muelle libre */

        /* 3. EJECUCION: ocupa el muelle */
        c->t_inicio = tiempo_actual();
        cambiar_estado(ctx, id, ESTADO_EJECUCION);

        char msg[64];
        snprintf(msg, sizeof(msg), "Ocupando muelle | burst=%d seg", c->burst);
        log_escribir(ctx, id, msg);

        /* ── SECCIÓN CRÍTICA ── trabajo de carga ── */
        sleep(c->burst);
        /* ── FIN SECCIÓN CRÍTICA ────────────────── */

        sem_post(&ctx->sem_muelles);   /* libera el muelle */

    /* ── ROUND ROBIN ──────────────────────────────────────────────────
       Cada camión toma el muelle por QUANTUM_RR segundos como máximo.
       Si no terminó, vuelve a la cola (re-encola) y espera otro turno.
       ───────────────────────────────────────────────────────────────── */
    } else { /* ALGORITMO_RR */

        while (c->burst_restante > 0) {

            cambiar_estado(ctx, id, ESTADO_BLOQUEADO);
            sem_wait(&ctx->sem_muelles);

            c->t_inicio = tiempo_actual();
            cambiar_estado(ctx, id, ESTADO_EJECUCION);

            int tiempo_uso = (c->burst_restante < QUANTUM_RR)
                             ? c->burst_restante
                             : QUANTUM_RR;

            char msg[80];
            snprintf(msg, sizeof(msg),
                     "Usando muelle | quantum=%d seg | restante=%d seg",
                     tiempo_uso, c->burst_restante);
            log_escribir(ctx, id, msg);

            /* ── SECCIÓN CRÍTICA ── */
            sleep(tiempo_uso);
            /* ───────────────────── */

            c->burst_restante -= tiempo_uso;
            sem_post(&ctx->sem_muelles);

            if (c->burst_restante > 0) {
                /* No terminó: regresa a la cola → estado LISTO */
                cambiar_estado(ctx, id, ESTADO_LISTO);
                log_escribir(ctx, id, "Quantum agotado, regresando a cola");
                cola_encolar(&ctx->cola, id);
                sleep(1); /* pequeño delay para ceder el semáforo */
            }
        }
    }

    /* 4. TERMINADO */
    c->t_fin    = tiempo_actual();
    c->t_retorno = c->t_fin - c->t_llegada;
    c->t_espera  = c->t_retorno - c->burst;
    if (c->t_espera < 0) c->t_espera = 0;

    cambiar_estado(ctx, id, ESTADO_TERMINADO);

    return NULL;
}
