#include "../include/terminal.h"

void log_init(ContextoTerminal *ctx) {
    ctx->archivo_log = fopen(LOG_FILE, "w");
    if (!ctx->archivo_log) {
        perror("No se pudo abrir el archivo de log");
        exit(EXIT_FAILURE);
    }
    fprintf(ctx->archivo_log, "=== LOG DE OPERACIONES - TERMINAL DE CARGA ===\n\n");
    fflush(ctx->archivo_log);
}

/* Escritura thread-safe: protegida por mutex_log */
void log_escribir(ContextoTerminal *ctx, int id_camion, const char *evento) {
    double t = tiempo_actual();
    char linea[256];
    snprintf(linea, sizeof(linea), "[t=%.3f] Camion-%02d | %s\n", t, id_camion, evento);

    /* --- SECCIÓN CRÍTICA: mutex protege el recurso compartido (log) --- */
    pthread_mutex_lock(&ctx->mutex_log);
    fputs(linea, ctx->archivo_log);
    fflush(ctx->archivo_log);
    printf("%s", linea);           /* eco en consola también */
    pthread_mutex_unlock(&ctx->mutex_log);
    /* ------------------------------------------------------------------ */
}

void log_cerrar(ContextoTerminal *ctx) {
    if (ctx->archivo_log) {
        fprintf(ctx->archivo_log, "\n=== FIN DEL LOG ===\n");
        fclose(ctx->archivo_log);
        ctx->archivo_log = NULL;
    }
}
