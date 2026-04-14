#ifndef TERMINAL_H
#define TERMINAL_H

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ───────────────────────────── CONSTANTES ───────────────────────────── */

#define MAX_CAMIONES       20
#define NUM_MUELLES        3
#define QUANTUM_RR         2   /* segundos de quantum para Round Robin   */
#define LOG_FILE           "logs/operaciones.log"

/* ───────────────────────────── ENUMERACIONES ────────────────────────── */

/* Estados del ciclo de vida de un hilo (camión) */
typedef enum {
    ESTADO_NUEVO,
    ESTADO_LISTO,
    ESTADO_EJECUCION,
    ESTADO_BLOQUEADO,
    ESTADO_TERMINADO
} EstadoHilo;

/* Tipos de planificación disponibles */
typedef enum {
    ALGORITMO_FIFO,
    ALGORITMO_RR
} AlgoritmoPlanificacion;

/* Tipo de carga (afecta prioridad) */
typedef enum {
    CARGA_NORMAL     = 1,
    CARGA_PERECEDERA = 0   /* prioridad más alta → número más bajo */
} TipoCarga;

/* ───────────────────────────── ESTRUCTURAS ──────────────────────────── */

/* Representa un camión (hilo) */
typedef struct {
    int             id;
    TipoCarga       tipo;
    int             burst;          /* tiempo de CPU necesario (seg)    */
    int             burst_restante; /* usado en Round Robin             */
    int             prioridad;      /* 0 = alta, 1 = baja               */
    EstadoHilo      estado;

    /* Métricas de tiempo */
    double          t_llegada;
    double          t_inicio;
    double          t_fin;
    double          t_espera;
    double          t_retorno;
} Camion;

/* Cola de planificación (FIFO / RR comparten la misma estructura) */
typedef struct {
    int  ids[MAX_CAMIONES]; /* IDs de camiones en cola               */
    int  frente;
    int  fin;
    int  tamanio;
    pthread_mutex_t mutex_cola;
} ColaPlanificacion;

/* Contexto global compartido por todos los hilos */
typedef struct {
    Camion              camiones[MAX_CAMIONES];
    int                 num_camiones;

    ColaPlanificacion   cola;
    sem_t               sem_muelles;     /* controla acceso a los N muelles */
    pthread_mutex_t     mutex_log;       /* protege escritura en el log      */
    pthread_mutex_t     mutex_estado;    /* protege cambios de estado        */

    AlgoritmoPlanificacion algoritmo;
    FILE               *archivo_log;

    /* Sincronización del planificador */
    pthread_cond_t      cond_planificador;
    pthread_mutex_t     mutex_planificador;
    int                 planificador_activo;
} ContextoTerminal;

/* ──────────────────────── PROTOTIPOS DE FUNCIONES ───────────────────── */

/* terminal.c  – núcleo del sistema */
void  terminal_init(ContextoTerminal *ctx, AlgoritmoPlanificacion alg, int num_camiones);
void  terminal_run(ContextoTerminal *ctx);
void  terminal_destroy(ContextoTerminal *ctx);
void  terminal_imprimir_resumen(ContextoTerminal *ctx);

/* camion.c  – ciclo de vida del hilo */
void *hilo_camion(void *arg);

/* planificador.c  – lógica FIFO / RR */
void  cola_init(ColaPlanificacion *cola);
void  cola_encolar(ColaPlanificacion *cola, int id);
int   cola_desencolar(ColaPlanificacion *cola);
int   cola_vacia(ColaPlanificacion *cola);

/* log.c  – registro protegido por mutex */
void  log_init(ContextoTerminal *ctx);
void  log_escribir(ContextoTerminal *ctx, int id_camion, const char *evento);
void  log_cerrar(ContextoTerminal *ctx);

/* utils.c  – utilidades */
double tiempo_actual(void);
const char *estado_str(EstadoHilo estado);
void  cambiar_estado(ContextoTerminal *ctx, int id, EstadoHilo nuevo);

#endif /* TERMINAL_H */
