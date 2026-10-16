/* tasks.c - exemplo do SnowOS Native: produtor/consumidor com fila, tarefas de prioridades diferentes e semáforo.
 *   build/snowcc os/apps/tasks.c -I os -I os/include --run */
#include "snowos_all.c"

Queue q; unsigned qbuf[4]; Sem done;
volatile unsigned checksum;

static void producer(void) {
  unsigned i;
  for (i = 1; i <= 10; i++) { queue_send(&q, i * i); os_printf("[%d ms] produziu %d\n", os_ticks(), i * i); task_sleep(2); }
  queue_send(&q, 0);
}
static void consumer(void) {
  unsigned v;
  while ((v = queue_recv(&q)) != 0) { checksum += v; os_printf("[%d ms]   consumiu %d\n", os_ticks(), v); }
  sem_post(&done);
}
void app_main(void) {
  queue_init(&q, qbuf, 4); sem_init(&done, 0);
  task_create(producer, 2048, 5); task_create(consumer, 2048, 6);
  sem_wait(&done);
  os_printf("checksum = %d (esperado 385)\n", checksum);
  os_exit(checksum == 385 ? 0 : 1);
}
