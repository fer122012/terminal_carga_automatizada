#include "../include/terminal.h"

/* Retorna tiempo en segundos (punto flotante) desde epoch */
double tiempo_actual(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* String legible del estado de un hilo */
const char *estado_str(EstadoHilo estado) {
    switch (estado) {
        case ESTADO_NUEVO:      return "NUEVO";
        case ESTADO_LISTO:      return "LISTO";
        case ESTADO_EJECUCION:  return "EJECUCION";
        case ESTADO_BLOQUEADO:  return "BLOQUEADO";
        case ESTADO_TERMINADO:  return "TERMINADO";
        default:                return "DESCONOCIDO";
    }
}

/* Cambia el estado de un camión de forma thread-safe y lo registra */
void cambiar_estado(ContextoTerminal *ctx, int id, EstadoHilo nuevo) {
    pthread_mutex_lock(&ctx->mutex_estado);
    EstadoHilo anterior = ctx->camiones[id].estado;
    ctx->camiones[id].estado = nuevo;
    pthread_mutex_unlock(&ctx->mutex_estado);

    if (anterior != nuevo) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Estado: %s -> %s",
                 estado_str(anterior), estado_str(nuevo));
        log_escribir(ctx, id, msg);
    }
}
