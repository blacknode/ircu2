# Propuesta 002 — Concurrencia: del event loop monohilo a ejecución paralela

**Estado:** aceptada — opción B implementada (fases 1, 2, 3 y 6)
**Rama base:** `main` (`0200c5d`)
**Alcance:** permitir que el servidor ejecute trabajo en paralelo (HTTP, servicios
embebidos, clientes de base de datos o de cola de mensajes) sin depender de procesos
externos

---

## 0. Estado de la implementación

Se implementó la **opción B**. La API vive en `include/worker.h` y
`ircd/worker.c`, y está documentada en `doc/readme.workers`.

| Fase | Estado | Dónde |
|---|---|---|
| 1 — pool, colas, integración por self-pipe | Hecha | `ircd/worker.c` |
| 2 — feature `WORKER_THREADS` (0 = desactivado) | Hecha | `FEAT_WORKER_THREADS`, `FEAT_WORKER_QUEUE_MAX` |
| 3 — worker de ejemplo + prueba | Hecha (prueba unitaria) | `modules/worker_demo.c`, `modules/worker_ticker.c`, `ircd/test/worker_t.c` |
| 4 — primer worker real | Pendiente | — |
| 5 — canal de comandos worker → núcleo | Pendiente | — |
| 6 — documentar la frontera | Hecha | `doc/readme.workers` |

Diferencias respecto de lo propuesto en §4.3, todas por cosas que cambiaron
desde que se escribió este documento o que aparecieron al implementarlo:

- **Dos formas, no una.** Además del pool de tareas de §4.3, hay workers
  dedicados (`worker_spawn()`): un hilo con bucle propio, que es la forma
  que pedía el caso "servidor HTTP" de §4.4 y que el pool no cubre.
- **La API es de módulos.** La propuesta es anterior a la estabilización de
  la API de módulos (propuesta 001). `module_submit_work()` y
  `module_spawn_worker()` contabilizan el trabajo contra el `ModuleHandle`,
  de modo que descargar un módulo cancela lo suyo y espera lo que ya corre
  — no hay alternativa correcta a esperar, porque el código del worker está
  a punto de desmapearse. Esto subió `IRCU_MODULE_ABI` de 3 a 4.
- **El cliente se referencia por numnick *y* por `cli_firsttime()`.** §4.3
  proponía sólo el numnick; los numnicks se reutilizan, y comparar también
  el instante de conexión evita el caso —raro pero silencioso— de contestarle
  a otro usuario.
- **Self-pipe, no `eventfd`.** Un descriptor más y funciona en los cinco
  motores sin condicionales. Cambiarlo a `eventfd` en Linux es una
  optimización local si alguna vez importa.
- **`worker_log()`.** No estaba en la propuesta. `log_write()` no es
  thread-safe, y "no loguees desde un worker" es una regla que se rompe por
  costumbre; darle una función que sí puede usar la quita del camino.

Sobre §8, pregunta 1 ("¿cuál es el caso de uso número uno?"): sigue abierta.
La frontera se diseñó sin un consumidor real, así que la fase 4 puede
descubrir que le falta algo.

---

## 1. Objetivo y encuadre

El objetivo declarado es **poder correr cosas en paralelo dentro del ircd**: un
servidor HTTP, servicios embebidos, clientes de Redis o Kafka. Los ejemplos son
ilustrativos, no una lista cerrada.

Hay dos formas muy distintas de llegar ahí, y conviene separarlas desde el principio
porque tienen un orden de magnitud de diferencia en coste y riesgo:

- **A — Paralelizar el núcleo IRC**: que varios hilos procesen clientes y estado de
  la red simultáneamente.
- **B — Paralelizar alrededor del núcleo**: el núcleo IRC sigue siendo monohilo, y
  el trabajo auxiliar corre en hilos aparte que se comunican con él por cola.

La opción B entrega el objetivo declarado. La opción A entrega, además, escalado
del propio procesamiento IRC a varios cores — que no es lo que se pidió.

Este documento analiza las dos y recomienda una. La sección §3 es el argumento
central; si sólo se lee una sección, que sea ésa.

---

## 2. Estado actual, con números

| Dato | Valor |
|---|---|
| Líneas en `ircd/` + `include/` | ~79.700 |
| Comandos en `msgtab` | 97 |
| Globales `extern` no-función en cabeceras | 65 |
| Buffers `static` compartidos en `ircd/*.c` | 31 |
| Motores de eventos a mantener | 5 (`epoll`, `kqueue`, `devpoll`, `poll`, `select`) |
| Hilos actuales | 0 (no hay un solo `pthread` en el árbol) |

El bucle es el clásico de un reactor: `event_loop()` (`ircd/ircd_events.c:373`)
delega en el motor, y el motor —por ejemplo `engine_loop()` en
`ircd/engine_epoll.c:202`— hace `epoll_wait()`, actualiza `CurrentTime`, y recorre
los descriptores listos generando eventos.

Y `event_generate()` no encola: **ejecuta el callback en línea**
(`ircd/ircd_events.c:180`):

```c
#ifndef IRCD_THREADED
/** we synchronously execute the event when not threaded */
#define event_add(event)  do { ... event_execute(_ev); } while (0)
```

### 2.1 El intento previo

Hay andamiaje de hilos abandonado en el árbol. `evInfo` tiene campos
`genq_head`/`genq_tail`/`genq_count` bajo `#ifdef IRCD_THREADED`, y existe una
implementación alternativa de `event_add()` que encola en vez de ejecutar. Lleva
estos comentarios, textualmente (`ircd/ircd_events.c:194-195`):

```c
/* This is just a placeholder; don't expect ircd to be threaded soon */
/* There should be locks all over the place in here */
```

Nadie definió nunca `IRCD_THREADED`. El andamiaje es útil como punto de partida
conceptual, pero no es código funcional: no hay locks, no hay work crew, y el
comentario `/* We'd also have to signal the work crew here */` (línea 238) marca
justamente la pieza que falta.

---

## 3. Por qué la opción A es un proyecto distinto del que parece

El problema no es el bucle de eventos. Es que **el estado del servidor es global y
mutable por todas partes**, sin ninguna disciplina de propiedad. Éstos son los
bloques que habría que resolver, con ubicación concreta:

### 3.1 Tablas hash globales sin sincronización

```c
/* ircd/hash.c:58 */ static struct Client  *clientTable[HASHSIZE];
/* ircd/hash.c:60 */ static struct Channel *channelTable[HASHSIZE];
```

Toda búsqueda de nick o canal pasa por aquí. Cada `NICK`, `JOIN` y `QUIT` las
modifica. Es el camino más caliente del servidor y no tiene ni un lock.

### 3.2 Free lists globales — cuatro subsistemas distintos

```c
/* ircd/list.c:61  */ static struct Client*     clientFreeList;
/* ircd/list.c:64  */ static struct Connection* connectionFreeList;
/* ircd/list.c:67  */ static struct SLink*      slinkFreeList;
/* ircd/msgq.c:77  */ MQData.msgs.free            /* pool de Msg   */
/* ircd/msgq.c:84  */ MQData.msgBufs[...].free    /* pool de MsgBuf */
/* ircd/IPcheck.c:91 */ static struct IPRegistryEntry* freeList;
/* ircd/ircd_events.c:96 */ evInfo.events_free    /* pool de Event */
```

Cada uno es un `push`/`pop` sobre una lista enlazada sin protección. Son de los
sitios más fáciles de "arreglar" con un mutex y de los peores para el rendimiento
si se hace así: se convierten en el punto de contención de todo el servidor.

### 3.3 Treinta y un buffers estáticos compartidos

```c
/* ircd/ircd_string.c:105 */ static char cbuf[BUFSIZE];
/* ircd/ircd_string.c:374 */ static char buf[SOCKIPLEN];
/* ircd/hash.c:369        */ static char temp[BUFSIZE + 1];
/* ircd/channel.c:1245    */ static char retmask[NICKLEN + USERLEN + HOSTLEN + 3];
/* ircd/s_user.c:516      */ static char umodeBuf[BUFSIZE];
/* ... 26 más */
```

Son funciones que devuelven un puntero a un buffer estático — patrón perfectamente
correcto en un programa monohilo y radiactivo en uno multihilo. Cada una es un
`__thread` o una reescritura de firma, y cada reescritura toca a todos sus
llamantes.

### 3.4 Estado temporal global

`CurrentTime` la escribe el motor tras cada `epoll_wait()`. Con varios hilos de
motor, ¿quién la escribe? ¿Cada hilo la suya? Toda la lógica de expiración
(G-lines, timers, IPcheck, whowas) la lee asumiendo un único valor coherente.

### 3.5 El orden de los mensajes es semántica, no detalle

Esto es lo más difícil y lo que no se arregla con locks. El protocolo IRC tiene
requisitos de orden: un `JOIN` debe llegar antes que el `PRIVMSG` que le sigue; un
`QUIT` debe llegar después de todo lo que el usuario envió. Con procesamiento
paralelo por cliente, dos mensajes de clientes distintos al mismo canal pueden
salir en orden invertido hacia parte de la red.

Y la resolución de colisiones de nick y el *riding* de canales en netsplits dependen
de comparaciones de timestamp que asumen un orden total de eventos en el servidor.
Paralelizar eso no es meter mutexes: es rediseñar los invariantes del protocolo.

### 3.6 Estimación honesta

No es "difícil pero acotado". Es un rediseño del modelo de memoria de 79.700 líneas
con una cola indefinida de *races* que sólo aparecen bajo carga real, en producción,
de forma no reproducible. Los ircd que lo han intentado en serio o bien lo hicieron
desde cero con esa premisa, o bien acabaron con un "gran lock" que anula la ganancia.

El coste realista está en el orden de **meses de trabajo a tiempo completo** y, lo
más caro, un período largo de inestabilidad en el que el servidor es peor que el
actual. Contra un beneficio que —para el objetivo declarado— no hace falta.

---

## 4. Opción B — Núcleo monohilo, periferia paralela (recomendada)

La observación que lo cambia todo: **HTTP, servicios embebidos, Redis y Kafka no
necesitan tocar el estado del núcleo IRC en paralelo.** Necesitan correr en paralelo
*con* él, y comunicarse con él en puntos bien definidos.

Eso se resuelve sin tocar el modelo de memoria del núcleo.

### 4.1 Arquitectura

```
        ┌──────────────────────────────────────────┐
        │  Hilo principal — núcleo IRC             │
        │  event_loop(), clientes, canales, P10    │
        │  SIN CAMBIOS en su modelo de memoria     │
        └───────▲──────────────────────┬───────────┘
                │ cola de respuestas   │ cola de tareas
                │ + eventfd (despierta │
                │   el motor)          │
        ┌───────┴──────────────────────▼───────────┐
        │  Pool de workers (N hilos)               │
        │  ┌────────┐ ┌────────┐ ┌──────────────┐  │
        │  │ HTTP   │ │  DB    │ │ servicios    │  │
        │  │ server │ │ libpq  │ │ embebidos    │  │
        │  └────────┘ └────────┘ └──────────────┘  │
        └──────────────────────────────────────────┘
```

**Regla única e innegociable:** un worker **nunca** toca `struct Client`,
`struct Channel`, las tablas hash, ni ninguna global del núcleo. Se comunica por
mensajes. Todo lo que necesita, lo recibe copiado en la tarea; todo lo que produce,
lo devuelve copiado en la respuesta.

Esa regla es lo que hace que la propuesta sea revisable: no hay que auditar 79.700
líneas, hay que auditar la frontera.

### 4.2 El despertador ya existe en el árbol

El núcleo ya sabe despertarse desde fuera de su hilo. Las señales usan el patrón
*self-pipe* (`ircd/ircd_events.c:292` y `:366`):

```c
static void signal_handler(int sig) {
  unsigned char c = (unsigned char) sig;
  write(sigInfo.fd, &c, 1);      /* async-signal-safe */
}
/* y el extremo de lectura es un Socket normal en el motor */
socket_add(&sigInfo.sock, signal_callback, 0, SS_NOTSOCK, SOCK_EVENT_READABLE, p[0]);
```

Un worker que termina una tarea hace exactamente lo mismo: encola la respuesta y
escribe un byte en el pipe (o `eventfd` en Linux). El motor lo ve como cualquier
otro descriptor listo, y el callback drena la cola **en el hilo principal**, donde
sí puede tocar el estado del núcleo.

No hay que modificar ninguno de los 5 motores.

### 4.3 API propuesta

```c
/* include/worker.h */

struct WorkTask;

/* Corre EN EL WORKER. No puede tocar estado del núcleo. */
typedef void (*WorkFn)(struct WorkTask *task);

/* Corre EN EL HILO PRINCIPAL, al drenar la cola de respuestas. */
typedef void (*WorkDoneFn)(struct WorkTask *task);

struct WorkTask {
  WorkFn       wt_work;
  WorkDoneFn   wt_done;

  /* Referencia segura al cliente: numnick, NO puntero.
   * El cliente puede haberse desconectado mientras el worker trabajaba. */
  char         wt_client[6];

  void        *wt_in;      /* entrada, propiedad del worker */
  size_t       wt_in_len;
  void        *wt_out;     /* salida, propiedad del hilo principal */
  size_t       wt_out_len;

  int          wt_status;
};

int  worker_init(int nthreads);
int  worker_submit(struct WorkTask *task);
void worker_shutdown(void);
```

**El detalle que evita la clase de bug más común:** `wt_client` guarda el numnick
(5 caracteres, `include/numnicks.h:50`), no un `struct Client *`. Cuando la
respuesta llega, el callback hace `findNUser()`; si el cliente ya no está, la
respuesta se descarta. Guardar el puntero sería un uso-tras-liberar esperando a
ocurrir, porque los clientes se liberan a listas libres que se reutilizan.

### 4.4 Cómo encaja cada caso de uso

| Caso | Dónde vive | Cómo cruza la frontera |
|---|---|---|
| Servidor HTTP | Worker con su propio `listen()`/loop | Sólo cruza para consultar estado: encola tarea, el hilo principal responde |
| Consulta a PostgreSQL | Worker, libpq **síncrono** (simple) | `wt_out` con el resultado; `wt_done` lo aplica |
| Publicar en Kafka/Redis | Worker, fire-and-forget | Sin respuesta, o sólo estado de error |
| Servicios embebidos | Worker con su estado propio | Genera comandos P10 encolados hacia el hilo principal |

Nótese que esto **resuelve el dilema de libpq** que quedó abierto en la discusión de
base de datos: en un worker, la API síncrona de libpq es perfectamente aceptable —
bloquea al worker, no al servidor. Desaparece la necesidad de la máquina de estados
asíncrona.

### 4.5 Servicios embebidos: la parte que sí toca el núcleo

Un servicio embebido necesita hacer cosas que sólo el hilo principal puede hacer:
introducir un pseudo-cliente, enviarle mensajes, cambiar modos. La solución es la
simétrica de la anterior: el worker no las hace, las **encola como comandos**, y el
hilo principal las ejecuta al drenar.

En la práctica el servicio embebido se comporta como un servidor P10 conectado por
un socket interno en memoria, en vez de por TCP. Eso tiene una propiedad valiosa:
**el mismo código de servicios funciona embebido o como nodo externo**, cambiando
sólo el transporte. Y permite desarrollarlo primero como nodo externo (más fácil de
depurar) y embeberlo después sin reescribirlo.

---

## 5. Comparativa

| | A — Núcleo multihilo | B — Periferia paralela |
|---|---|---|
| Cumple el objetivo declarado | Sí | Sí |
| Escala el procesamiento IRC a N cores | Sí | No |
| Líneas de núcleo a auditar | ~79.700 | ~0 (sólo la frontera) |
| Los 5 motores de eventos | Reescribir | Sin tocar |
| Riesgo de *races* en producción | Alto y prolongado | Acotado a la frontera |
| Reversible | No | Sí (se apaga con `nthreads = 0`) |
| Esfuerzo | Meses, con cola indefinida | Semanas |
| Permite libpq síncrono | Sí | Sí |

La opción A sólo gana en una fila: escalar el procesamiento IRC a varios cores. Y
para el volumen de una red IRC —donde el cuello de botella real es la E/S de red y
el ancho de banda, no la CPU de parseo— esa fila rara vez es la que decide.

**Recomendación: opción B.** Si en el futuro aparece evidencia medida de que el
parseo IRC satura un core, la opción A sigue disponible, y B no cierra la puerta.

---

## 6. Plan de trabajo (opción B)

| Fase | Contenido | Notas |
|---|---|---|
| 1 | `worker.h`/`worker.c`: pool, colas, integración por `eventfd`/self-pipe | El núcleo no cambia |
| 2 | Feature `WORKER_THREADS` (0 = desactivado, comportamiento actual bit a bit) | Interruptor de seguridad |
| 3 | Worker de prueba (echo con retardo) + prueba de integración | Valida la frontera |
| 4 | Primer worker real — el más simple de los casos de uso | Probablemente el cliente de DB |
| 5 | Canal de comandos worker → núcleo (para servicios embebidos) | La mitad difícil |
| 6 | Documentar la frontera en `doc/readme.workers` | Sin esto, la regla §4.1 se erosiona |

Con `WORKER_THREADS = 0` el servidor debe comportarse **exactamente** como hoy: sin
hilos creados, sin colas, sin `eventfd`. Ése es el criterio de aceptación de la fase
2 y la red de seguridad de todo lo demás.

Requisitos de construcción: `find_package(Threads REQUIRED)` y
`target_link_libraries(ircd PRIVATE Threads::Threads)` — depende de la migración a
CMake (rama `claude/autotools-cmake-migration-6uod0q`).

---

## 7. Riesgos de la opción B

| Riesgo | Mitigación |
|---|---|
| La regla "el worker no toca el núcleo" se erosiona con el tiempo | Documentarla; revisar cada PR que añada un worker contra ella |
| Cola de respuestas crece sin límite si el hilo principal va lento | Tamaño máximo y política explícita de descarte por tipo de tarea |
| Punteros a `struct Client` colados en una tarea | El API sólo acepta numnick; revisión de código |
| `MyMalloc`/`MyFree` desde un worker | Los pools de `list.c` y `msgq.c` no son thread-safe: los workers usan `malloc`/`free` del sistema para sus datos, no los pools del núcleo |
| Depuración de `DEBUGMODE`/`MDEBUG` con hilos | `memdebug.c` no es thread-safe; documentar que los workers no usan sus macros |

El cuarto punto merece énfasis: **los workers no pueden usar `MyMalloc`**. Es una
regla fácil de violar por costumbre y de las que producen corrupción silenciosa.
Conviene que el código de workers viva en un directorio propio y que una prueba de
CI busque esos símbolos en él.

---

## 8. Preguntas abiertas

1. **¿Cuál es el caso de uso número uno?** El diseño de la frontera se afina mejor
   con un consumidor real que en abstracto.
2. **¿Hay medición de que la CPU sea el cuello de botella hoy?** Si la hay, cambia
   la comparativa de §5 y merece la pena reconsiderar A.
3. **Servicios embebidos: ¿en vez de nodo externo, o además?** Si es "además",
   conviene que compartan el código de protocolo desde el principio.
4. **¿Se acepta que la opción B no escale el parseo IRC a varios cores?** Es la
   contrapartida explícita de la recomendación.

---

## 9. Nota sobre la relación con la propuesta 001

Las dos propuestas son independientes y pueden hacerse en cualquier orden, pero
interactúan en un punto: **si ambas se implementan, los hooks de módulos corren en
el hilo principal**, igual que hoy. Un módulo que quiera hacer trabajo bloqueante
debe encolarlo a un worker, y para eso necesita un hook que pueda suspender la
operación y reanudarla — un `HOOK_ASYNC` que no está en la propuesta 001.

Si se prevé ese caso (por ejemplo, un módulo antispam que consulte una API externa
antes de permitir un mensaje), conviene decidirlo antes de congelar el ABI de
módulos, porque añadirlo después lo rompe.
