#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include "config.h"
#include "chieftain.h"
#include "valhalla.h"

void chieftain_init(chieftain_t *self, valhalla_t *valhalla)
{
    self->valhalla = valhalla;

    /* cria o array de cadeiras cada posicao tem como ser:
     *   -1 = vazia
     *    0 = ocupada por viking normal
     *    1 = ocupada por berserker */
    self->seats = malloc(sizeof(int) * config.table_size);
    for (int i = 0; i < (int) config.table_size; i++)
        self->seats[i] = -1;

    /* mutex e cond para controlar acesso a mesa */
    pthread_mutex_init(&self->table_mutex, NULL);
    pthread_cond_init(&self->table_cond, NULL);

    /* contador do banquete:
     * banquet_done  = quantos vikings ja terminaram de comer
     * banquet_total = quantos vikings precisam comer (todos os normais)
     * quando done == total, o banquete acabou e as preces podem comecar */
    self->banquet_done  = 0;
    self->banquet_total = config.horde_size;
    pthread_mutex_init(&self->banquet_mutex, NULL);
    pthread_cond_init(&self->banquet_cond, NULL);

    pthread_mutex_init(&self->god_mutex, NULL);

    plog("[chieftain] Initialized\n");
}

int chieftain_acquire_seat_plates(chieftain_t *self, int berserker)
{
    int cadeira_escolhida = -1;

    pthread_mutex_lock(&self->table_mutex);

    /* fica em loop ate conseguir uma cadeira valida */
    while (cadeira_escolhida == -1) {

        /* tenta cada cadeira da mesa */
        for (int i = 0; i < (int) config.table_size; i++) {

            /* regra 1: cadeira tem que estar vazia */
            if (self->seats[i] != -1)
                continue;

            /* olha para quem sao os vizinhos na mesa*/
             
            int vizinho_esq = (i - 1 + config.table_size) % config.table_size;
            int vizinho_dir = (i + 1) % config.table_size;

            /* regra 2: nao pode sentar ao lado do tipo oposto.
             * se o vizinho esta vazio (-1) vai
             * se o vizinho e do mesmo tipo vao
             * se o vizinho e do tipo oposto, nao pode sentar aqui */
            int esq_ok = (self->seats[vizinho_esq] == -1 || self->seats[vizinho_esq] == berserker);
            int dir_ok = (self->seats[vizinho_dir] == -1 || self->seats[vizinho_dir] == berserker);

            if (!esq_ok || !dir_ok)
                continue;

            /* cadeira passou nas duas regras escolhe ela */
            cadeira_escolhida = i;
            break;
        }

        /* nao encontrou nenhuma cadeira valida agora
         * ele vai dormir e espera alguem levantar para tentar de novo */
        if (cadeira_escolhida == -1)
            pthread_cond_wait(&self->table_cond, &self->table_mutex);
    }

    /* marca a cadeira como ocupada pelo tipo desse viking */
    self->seats[cadeira_escolhida] = berserker;

    pthread_mutex_unlock(&self->table_mutex);

    plog("[chieftain] Viking (berserker=%d) sentou na cadeira %d\n", berserker, cadeira_escolhida);
    return cadeira_escolhida;
}

void chieftain_release_seat_plates(chieftain_t *self, int pos)
{
    /* libera a cadeira e acorda quem esta esperando por uma vaga */
    pthread_mutex_lock(&self->table_mutex);
    self->seats[pos] = -1;
    pthread_cond_broadcast(&self->table_cond);
    pthread_mutex_unlock(&self->table_mutex);

    /* 
     * registra que mais um viking terminou de comer
     * quando o ultimo terminar dispara o sinal para todos que estao esperando (a pessoa 2 vai ter que pensar nisso)
     * */
    pthread_mutex_lock(&self->banquet_mutex);
    self->banquet_done++;
    plog("[chieftain] Banquete: %d/%d terminaram de comer\n", self->banquet_done, self->banquet_total);

    if (self->banquet_done == self->banquet_total) {
        plog("[chieftain] Banquete encerrado! Preces podem comecar.\n");
        pthread_cond_broadcast(&self->banquet_cond);
    }

    pthread_mutex_unlock(&self->banquet_mutex);
    
}

god_t chieftain_get_god(chieftain_t *self)
{
    /* 
     * deve escolher um deus aleatorio respeitando as regras de tolerancia.
     * campos para vc usar que ja implentei
     *   god_mutex - mutex para usar para proteger o rand()
     *   valhalla->prayers[] - usei como contadores de preces por deus
     *   valhalla->pray_mutex - eu criei em valhalla.h para usar ao ler prayers[] */
    
    //barreira
    pthread_mutex_lock(&self->banquet_mutex);
    while (self->banquet_done < self->banquet_total) {
        pthread_cond_wait(&self->banquet_cond, &self->banquet_mutex);
    }
    pthread_mutex_unlock(&self->banquet_mutex);

    // escolha deus aleatorio
    god_t all_gods[8] = {BALDR, LOKI, VALI, HODER, FRIGG, JORD, ODIN, THOR};
    pthread_mutex_lock(&self->god_mutex);
    int start_index = rand() % 8;
    pthread_mutex_unlock(&self->god_mutex);

    god_t chosen_god = THOR; // deus padrão caso não encontre nenhum deus melhor
    pthread_mutex_lock(&self->valhalla->pray_mutex);
    for (int i = 0; i < 8; i++) {
        int index = (start_index + i) % 8;
        god_t current_god = all_gods[index];
        int is_valid = 0;

        if (current_god == THOR || current_god == ODIN) {
            // thor e odin: até 10% a mais que a soma dos outros 6
            int sum_6 = self->valhalla->prayers[BALDR] + self->valhalla->prayers[LOKI] +
                        self->valhalla->prayers[VALI]  + self->valhalla->prayers[HODER] +
                        self->valhalla->prayers[FRIGG] + self->valhalla->prayers[JORD];

            int my_count = self->valhalla->prayers[current_god] + 1;
            int tolerance = (int) ceil(sum_6 * 0.10);
            
            // anti-deadlock para quando todas as preces iniciarem zeradas
            if (tolerance == 0) tolerance = 1; 

            if (my_count <= sum_6 + tolerance) {
                is_valid = 1;
            }
        } else {
            //  deuses rivais
            god_t rival;
            if (current_god == BALDR) rival = LOKI;
            else if (current_god == LOKI) rival = BALDR;
            else if (current_god == VALI) rival = HODER;
            else if (current_god == HODER) rival = VALI;
            else if (current_god == FRIGG) rival = JORD;
            else if (current_god == JORD) rival = FRIGG;
            
            int my_count = self->valhalla->prayers[current_god] + 1;
            int rival_count = self->valhalla->prayers[rival];
            
            // Se o atual ficar atrás ou empatado, é sempre seguro
            if (my_count <= rival_count) {
                is_valid = 1; 
            } else {
                // Se for ultrapassar, ve se a diferença excede 5% (arredondando para cima)
                int diff = my_count - rival_count;
                int tolerance = (int) ceil(rival_count * 0.05);
                
                if (tolerance == 0) tolerance = 1; // para o zero absoluto

                if (diff <= tolerance) {
                    is_valid = 1;
                }
            }
        }

        // se encontrou um deus válido, registra a prece, sai do loop e libera o mutex
        if (is_valid) {
            self->valhalla->prayers[current_god]++;
            chosen_god = current_god;
            break;
        }
    }

    pthread_mutex_unlock(&self->valhalla->pray_mutex);

    return chosen_god;
}

void chieftain_finalize(chieftain_t *self)
{
    free(self->seats);

    pthread_mutex_destroy(&self->table_mutex);
    pthread_cond_destroy(&self->table_cond);

    pthread_mutex_destroy(&self->banquet_mutex);
    pthread_cond_destroy(&self->banquet_cond);

    pthread_mutex_destroy(&self->god_mutex);

    plog("[chieftain] Finalized\n");
}