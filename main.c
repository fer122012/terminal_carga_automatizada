#include "include/terminal.h"

/*
 * Uso: ./terminal [fifo|rr] [num_camiones]
 * Ejemplos:
 *   ./terminal fifo 8
 *   ./terminal rr   6
 *   ./terminal          ← usa FIFO con 5 camiones por defecto
 */
int main(int argc, char *argv[]) {
    AlgoritmoPlanificacion alg         = ALGORITMO_FIFO;
    int                    num_camiones = 5;

    if (argc >= 2) {
        if (strcmp(argv[1], "rr") == 0 || strcmp(argv[1], "RR") == 0)
            alg = ALGORITMO_RR;
    }
    if (argc >= 3) {
        num_camiones = atoi(argv[2]);
        if (num_camiones < 1 || num_camiones > MAX_CAMIONES) {
            fprintf(stderr, "num_camiones debe estar entre 1 y %d\n",
                    MAX_CAMIONES);
            return EXIT_FAILURE;
        }
    }

    ContextoTerminal ctx;

    terminal_init(&ctx, alg, num_camiones);
    terminal_run(&ctx);
    terminal_imprimir_resumen(&ctx);
    terminal_destroy(&ctx);

    return EXIT_SUCCESS;
}
