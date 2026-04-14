#include "../include/terminal.h"

/* ── Inicialización ── */
void cola_init(ColaPlanificacion *cola) {
    cola->frente  = 0;
    cola->fin     = 0;
    cola->tamanio = 0;
    pthread_mutex_init(&cola->mutex_cola, NULL);
}

/* ── Encolar (al final) ── */
void cola_encolar(ColaPlanificacion *cola, int id) {
    pthread_mutex_lock(&cola->mutex_cola);
    if (cola->tamanio >= MAX_CAMIONES) {
        fprintf(stderr, "Cola llena, no se puede encolar camion %d\n", id);
        pthread_mutex_unlock(&cola->mutex_cola);
        return;
    }
    cola->ids[cola->fin] = id;
    cola->fin = (cola->fin + 1) % MAX_CAMIONES;
    cola->tamanio++;
    pthread_mutex_unlock(&cola->mutex_cola);
}

/* ── Desencolar (del frente) ── Retorna -1 si está vacía */
int cola_desencolar(ColaPlanificacion *cola) {
    pthread_mutex_lock(&cola->mutex_cola);
    if (cola->tamanio == 0) {
        pthread_mutex_unlock(&cola->mutex_cola);
        return -1;
    }
    int id = cola->ids[cola->frente];
    cola->frente = (cola->frente + 1) % MAX_CAMIONES;
    cola->tamanio--;
    pthread_mutex_unlock(&cola->mutex_cola);
    return id;
}

/* ── Verificar si está vacía ── */
int cola_vacia(ColaPlanificacion *cola) {
    pthread_mutex_lock(&cola->mutex_cola);
    int vacia = (cola->tamanio == 0);
    pthread_mutex_unlock(&cola->mutex_cola);
    return vacia;
}
