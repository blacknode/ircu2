# Propuesta 006 — De IRC a comunicaciones unificadas: texto enriquecido, voz, vídeo y pantalla compartida

**Estado:** hoja de ruta, en discusión
**Depende de:** 001 (API de módulos), 002 (hilos), 003 (modos por módulo),
004 (configuración desde el entorno), 005 (traducciones)
**Introduce:** nada todavía; define el orden en que se introduce lo demás

---

## 1. Qué se quiere y qué significa realmente

El objetivo es que esta base — un ircu2 con P10, modular — sostenga un producto
del tipo Slack/Teams: conversaciones con texto enriquecido, hilos, reacciones,
edición y borrado, historial buscable, ficheros adjuntos, y llamadas de voz,
vídeo y pantalla compartida.

Conviene decir de entrada lo que es cierto y lo que no:

- **Lo que IRC ya hace bien y no hay que reescribir:** enrutado de mensajes en
  tiempo real entre miles de clientes, pertenencia a salas, permisos por sala,
  federación entre servidores, límites anti-abuso, TLS, WebSocket. Esa es la
  mitad difícil de un Slack y ya está funcionando.
- **Lo que IRC no hace en absoluto:** recordar nada. Un servidor IRC es un
  conmutador sin memoria: si no estabas conectado, el mensaje no existió. Slack
  es, esencialmente, una base de datos de mensajes con una capa de tiempo real
  encima. **Esa inversión de prioridades es el trabajo de fondo de este
  roadmap**, no los comandos nuevos.
- **Lo que no va dentro del ircd nunca:** los medios. Un flujo de vídeo no
  pasa por P10 ni por el hilo del event loop. El ircd es **señalización y
  autorización**; los paquetes RTP van por un SFU aparte (§7.5).

---

## 2. Punto de partida: inventario de lo que ya hay

Esto no se empieza de cero. Lo ya construido cubre buena parte de los cimientos:

| Pieza | Dónde | Sirve para |
|---|---|---|
| API de módulos ABI 7 | `include/module.h` | Todo lo nuevo entra como módulo |
| Comandos desde módulo, con token P10 propio | `module_add_command()` | `RTC`, `HISTORY`, `REACT`… sin tocar el core |
| Modos de usuario y de canal desde módulo | 003, `chan_modes.c` | `+call-enabled`, `+rich`, roles |
| Hooks de ciclo de vida (17 puntos, veto y reescritura) | `include/hooks.h` | Autorización, captura, filtrado |
| Hilos de trabajo | `worker.c`, `FEAT_WORKER_THREADS` | HTTP saliente, hashing, colas |
| API de base de datos + driver PostgreSQL asíncrono | `db.c`, `modules/workers/postgres/` | **El almacén de mensajes ya tiene por dónde entrar** |
| Migraciones SQL versionadas por módulo | `migration.c`, `/MODULE MIGRATION` | Esquema del historial |
| Bots y servicios en proceso | `bot.c`, `Service{}`, `irc_services` | Bots, integraciones, webhooks entrantes |
| WebSocket RFC6455 sobre los listeners | `websocket.c`, `Port { websocket = yes; }` | El cliente web se conecta ya |
| TLS (OpenSSL/GnuTLS/libtls) | `ircd_tls.h` | Transporte |
| Tags IRCv3 + `TAGMSG` + `CLIENTTAGDENY` | `msg_tag.c`, `m_tagmsg.c` | Reacciones, typing, metadatos por mensaje |
| i18n con ficheros PO, core y módulos | 005 | Producto multi-idioma |
| Host virtual obligatorio (`+x`, TEA) | `ircd_vhost.c` | Privacidad de IP por defecto |

Capacidades IRCv3 anunciadas hoy: `away-notify`, `chghost`, `echo-message`,
`invite-notify`, `userhost-in-names`, `message-tags`, `server-time`,
`draft/languages`, `cap-notify`.

---

## 3. Las ocho brechas reales

### 3.1 No hay memoria
Nada persiste un mensaje. No hay identificador de mensaje, ni historial, ni
marca de leído, ni no-leídos, ni búsqueda. Un cliente que reconecta empieza en
blanco.

### 3.2 No hay identidad persistente
Las cuentas se retiraron deliberadamente (`doc/readme.accounting`): no hay
nombre de cuenta, ni SASL, ni `ACCOUNT`; `+r` significa sólo «identificado al
nick que usa» y lo pone un servicio. Un producto tipo Slack necesita lo
contrario: una identidad estable, independiente del nick, con SSO, invitaciones
y roles. **Esta es la decisión estructural del proyecto** (§6).

### 3.3 La línea es de 512 bytes
`BUFSIZE` es 512 (`include/ircd_defs.h`), `TOPICLEN` 160, `AWAYLEN` 160,
`NICKLEN` 15, `CHANNELLEN` 200. El buffer de reescritura de los hooks de
mensaje es `char rewrite[BUFSIZE]` (`ircd_relay.c:147`). Los tags sí tienen
8191 bytes (`TAGSLEN`), pero el cuerpo no. Un párrafo con formato, una cita o
un bloque de código no caben.

### 3.4 `capset_t` es de 16 bits
`typedef unsigned short capset_t;` (`include/client.h:95`), y la lista de
capacidades es una macro-X en tiempo de compilación (`include/capab.h`). Hay
**9 capacidades usadas de 16 posibles: quedan 7 libres**, y un módulo no puede
registrar ninguna. Sólo las capacidades IRCv3 que este producto necesita
(`batch`, `labeled-response`, `message-ids`, `draft/chathistory`,
`draft/multiline`, `draft/message-redaction`, `draft/react`, `draft/reply`,
`draft/typing`, `draft/read-marker`, `standard-replies`, `sasl`, `setname`,
`metadata-2`, `draft/filehost`) son quince. **Esto bloquea todo lo demás y por
eso es la fase 0.**

### 3.5 Los hooks de mensaje sólo ven el origen local
`HOOK_MESSAGE_PRE_CHANNEL` y `HOOK_MESSAGE_PRE_PRIVATE` se disparan en
`relay_channel_message()` y `relay_private_message()` — mensajes de un cliente
*de este servidor*. Las rutas `server_relay_*` (mensaje que llega de otro
servidor) no los disparan. Un módulo de historial cargado en un servidor sólo
vería lo que se dijo en ese servidor. Hace falta un punto de captura en la
entrega, o un servidor de historial designado (§7.2).

### 3.6 Los hooks son síncronos
`HOOK_PENDING` está reservado pero no implementado (`hooks.h`). Todo hook
corre en línea en el hilo principal. Autorizar un `JOIN` contra la base de
datos, o validar un token SSO, no se puede hacer hoy sin bloquear el servidor
entero.

### 3.7 No hay HTTP
El WebSocket es sólo un *upgrade* que transporta líneas IRC. No hay endpoints
HTTP: no hay subida de ficheros, ni webhooks entrantes, ni API REST para
integraciones, ni descarga de adjuntos.

### 3.8 Un solo espacio de nombres plano
IRC tiene un espacio global de nicks de 15 caracteres y canales visibles a
toda la red. Slack tiene *workspaces* aislados, con nombres repetibles entre
ellos y un nombre de visualización libre por persona. El modelo no encaja tal
cual (§6.2).

---

## 4. Principio rector: qué va al core y qué va a un módulo

La regla que ya sigue el proyecto y que este roadmap mantiene:

> **Al core va el mecanismo; al módulo va la política.**

Va al core lo que *no puede* estar fuera: el formato de la línea, el registro
de capacidades, el enrutado, los identificadores de mensaje, el transporte, un
punto de extensión. Va a un módulo todo lo que decide *qué* se hace: el
almacén de historial, la autenticación, las reacciones, las llamadas, los
workspaces, las integraciones.

Corolario práctico: **cada fase del roadmap empieza por el trozo de core que
la habilita y termina en un módulo que la implementa.** Si una fase es sólo
core, no entrega valor; si es sólo módulo, probablemente choca con un límite
del core y hay que volver atrás.

---

## 5. Fase 0 — Cimientos (core). Sin esto no hay nada

Es la fase más aburrida y la que más determina todo lo demás. Todo aquí es
trabajo de core, y sube `IRCU_MODULE_ABI` a 8.

### 5.1 Capacidades dinámicas — **el bloqueo duro**
- `capset_t` de 16 bits → mapa de bits de tamaño fijo (como `flag_t` en
  `user_flags.h`), con 64–128 posiciones.
- Tabla de capacidades en tiempo de ejecución, igual que se hizo con los modos
  en 003: el core registra las suyas y `module_add_cap(mod, "draft/react",
  flags, &cap)` devuelve la posición. Se revierte al descargar el módulo, con
  `CAP DEL` a quien tenga `cap-notify`.
- `cap_new()`/`cap_del()`/`cap_update_availability()` ya existen: la
  notificación en caliente sale gratis.

### 5.2 Longitud de línea
- Capacidad `draft/multiline` + `BATCH`: es la respuesta estándar y federable
  a los 512 bytes, y no rompe a ningún cliente antiguo.
- Subir el buffer de reescritura de hooks por encima de `BUFSIZE`.
- Revisar `TOPICLEN`/`AWAYLEN` como *features*, no como constantes.

### 5.3 `BATCH` y `labeled-response`
`BATCH` es prerrequisito de multiline, de chathistory y de cualquier entrega
agrupada. `labeled-response` es lo que permite a un cliente web correlacionar
petición y respuesta — imprescindible para una UI que no sea un terminal.

### 5.4 Identificadores de mensaje (`msgid`)
Un tag `msgid` estable y único por red, generado en el servidor de origen y
federado en P10. **Es la clave primaria de todo lo que viene después**:
historial, respuestas en hilo, reacciones, ediciones, borrados, marcas de
leído. Sin `msgid` ninguna de esas cinco cosas se puede construir.

### 5.5 Hooks asíncronos (`HOOK_PENDING`)
Implementar el valor que ya está reservado: un hook devuelve `HOOK_PENDING`, la
operación queda suspendida con su contexto, y el módulo la reanuda
(`hook_resume(token, HOOK_ALLOW|HOOK_DENY)`) cuando su consulta a base de datos
o su verificación de token termina. Requiere congelar el estado del cliente
mientras tanto y un plazo máximo. Es lo que hace posible autenticar y autorizar
contra un almacén externo sin parar el servidor.

### 5.6 Puntos de extensión que faltan
- `module_add_isupport()` — anunciar tokens en el 005 desde un módulo.
- `module_add_feature()` — *features* propias, visibles en `/GET` y `/SET`.
- `module_add_config_block()` — bloques de configuración propios, en vez de
  ampliar `ircd_parser.y` por cada módulo.
- `module_add_numeric()` — numerics propios, con su contexto de traducción.
- Hooks nuevos: `HOOK_MESSAGE_DELIVERED` (incluido origen remoto, §3.5),
  `HOOK_CHANNEL_TOPIC_CHANGED`, `HOOK_CLIENT_AWAY`, `HOOK_PRESENCE_CHANGED`.

**Criterio de aceptación de la fase:** un módulo de ejemplo registra una
capacidad, un token ISUPPORT, una *feature* y un hook asíncrono, y `/MODULE
UNLOAD` lo revierte todo sin dejar rastro en ningún cliente conectado.

---

## 6. Fase 1 — Identidad. La decisión que condiciona el producto

### 6.1 La tensión
`doc/readme.accounting` documenta la retirada deliberada de cuentas, SASL y
`ACCOUNT`. Un producto tipo Slack no es viable sin identidad persistente:
sin ella no hay historial por persona, ni no-leídos, ni invitaciones, ni SSO
corporativo, ni permisos estables, ni sesiones en varios dispositivos.

**Recomendación:** reintroducir la identidad, pero **fuera del core y con otra
forma**, para no deshacer 005 ni volver al modelo que se retiró:

- El core mantiene `+r` y `+x` como están.
- Un módulo `identity` posee la tabla de cuentas (migraciones propias,
  PostgreSQL), implementa `sasl` (PLAIN, EXTERNAL por certificado, y `OAUTHBEARER`
  para SSO/OIDC) a través de un hook asíncrono en `HOOK_CLIENT_PRE_REGISTER`,
  y expone la cuenta como un tag `account` y un modo registrado por el módulo.
- El *nick* deja de ser la identidad: es el identificador de enrutado. El
  nombre visible va por `setname`/`metadata-2`.

Si la decisión es **no** reintroducir cuentas, el producto se queda en
«mensajería de equipo efímera, con llamadas» — que es un producto legítimo, y
entonces las fases 2 y 7 se recortan mucho. **Conviene decidirlo antes de
escribir la fase 0**, porque cambia qué se guarda en el `msgid`.

### 6.2 Workspaces
Dos caminos:

| | Una red IRC por workspace | Namespacing dentro de una red |
|---|---|---|
| Aislamiento | Total, gratis | Hay que imponerlo en cada hook |
| Nicks repetidos entre workspaces | Sí | No (espacio global) |
| Coste operativo | Un despliegue por cliente | Uno solo |
| Usuario en varios workspaces | Varias conexiones | Una |
| Encaja con P10 | Sí | A medias |

**Recomendación:** namespacing (`#ws-acme/general`) con un módulo `workspace`
que filtre `LIST`, `NAMES`, `WHO`, `WHOIS` e `INVITE` por pertenencia, y que
imponga el aislamiento en `HOOK_CHANNEL_PRE_JOIN`. Aceptando de entrada que
el nick es global y que el nombre visible es otra cosa.

---

## 7. Fases 2 a 7 — El producto

### 7.1 Fase 2 — Historial (`draft/chathistory`)
Módulo `history` + esquema propio en PostgreSQL.

- Captura en `HOOK_MESSAGE_DELIVERED` (fase 0), no en el hook de origen local.
- Escrituras a través del driver asíncrono; **nunca** en el hilo del event loop.
- Comandos `CHATHISTORY LATEST|BEFORE|AFTER|AROUND|BETWEEN|TARGETS`, entregados
  dentro de un `BATCH`.
- Retención y purga como *features*; borrado por usuario y por workspace (esto
  es requisito legal, no una mejora).
- **Decisión de topología:** o cada servidor escribe lo que entrega (y la
  deduplicación por `msgid` la hace la base de datos), o un servidor designado
  hace de archivador. Lo primero escala mejor y es lo recomendado.

### 7.2 Fase 3 — Semántica de conversación moderna
Todo esto son tags sobre `msgid`, y casi todo cabe en módulos una vez existe la
fase 0:

- **Hilos:** tag `+draft/reply=<msgid>`.
- **Reacciones:** `TAGMSG` con `+draft/react`, persistidas por el módulo
  `history`.
- **Edición y borrado:** `draft/message-redaction` (`REDACT`), con política de
  quién puede y durante cuánto tiempo.
- **Typing:** `+typing`, efímero, sin persistir.
- **Marcas de leído y no-leídos:** `draft/read-marker` (`MARKREAD`), por cuenta
  y por objetivo; es lo que convierte esto en una bandeja de entrada.
- **Menciones y notificaciones:** derivadas de la cuenta, no del nick.

### 7.3 Fase 4 — Texto enriquecido
El punto delicado: IRC no tiene tipo de contenido.

**Recomendación:** no inventar un dialecto de códigos de control. Negociar el
formato con una capacidad propia (`blacknode/richtext`) y llevar el cuerpo como
Markdown acotado (negrita, cursiva, tachado, código, bloque de código, cita,
lista, enlace, mención) señalizado con un tag `+blacknode/format=markdown`.

- Cliente sin la capacidad → recibe el texto plano equivalente, generado por el
  servidor. Esto es innegociable: rompe la red si no se cumple.
- El *saneado* (nada de HTML, límites de anidamiento y de longitud) se hace
  en el servidor, en un hook de reescritura, nunca en el cliente.
- Bloques largos → `draft/multiline` de la fase 0.

### 7.4 Fase 5 — Ficheros y adjuntos
Los blobs no pasan por P10. Hacen falta dos cosas:

1. **Un servicio HTTP** para subir y descargar, con almacenamiento de objetos
   detrás (S3 o compatible). Puede ser un proceso aparte que comparte la base
   de datos, o un módulo que abra su propio listener con `module_spawn_worker()`.
   *Recomendación: proceso aparte.* Meter un servidor HTTP completo en el hilo
   del ircd contradice el principio de §4 y añade una superficie de ataque
   grande dentro del proceso que sostiene la red.
2. **Autorización desde el ircd:** el ircd emite un token de subida de vida
   corta (HMAC) ligado a la cuenta y al canal; el servicio HTTP lo valida. El
   mensaje resultante lleva la URL y los metadatos en tags
   (`draft/filehost`). Miniaturas y antivirus, en *workers*.

### 7.5 Fase 6 — Voz, vídeo y pantalla compartida
Aquí está la separación más importante de todo el documento:

```
  Cliente  ──── señalización (WebSocket/IRC) ────►  ircd  (quién puede, a qué sala)
     │                                                │
     │                                                └─► emite credenciales TURN + token SFU
     │
     └──────────── medios (SRTP/DTLS, WebRTC) ────►  SFU (LiveKit / mediasoup / Janus)
                                                      │
                                                      └─► TURN (coturn) para NAT simétrico
```

**El ircd hace, y sólo hace:**
- Un módulo `rtc` con un comando propio y su token P10: `RTC START|JOIN|LEAVE|
  OFFER|ANSWER|CANDIDATE|END`, propagado por la red como cualquier comando.
- Autorización: quién puede iniciar una llamada en un canal y quién puede
  unirse sale de la pertenencia al canal y de los modos — el estado que el ircd
  ya tiene y nadie más tiene.
- Emisión de credenciales: credenciales TURN REST efímeras (HMAC-SHA1; hay
  `ircd/ircd_sha1.c`) y token de sala del SFU (LiveKit usa JWT HS256 — haría
  falta SHA-256 en el core o en el módulo).
- Estado de llamada visible para todos: quién está en la llamada, un modo de
  canal registrado por el módulo, presencia en la UI.

**El ircd no hace, nunca:** transportar RTP, transcodificar, grabar, mezclar.

**Pantalla compartida** no es una función aparte: es una pista de vídeo más en
WebRTC (`getDisplayMedia`), con su etiqueta. Si las llamadas funcionan, compartir
pantalla es trabajo de cliente y de SFU, no de servidor. Grabación y
transcripción, lo mismo: del SFU hacia afuera.

**Elección de SFU** (decisión a tomar en esta fase, no antes): LiveKit (la vía
más rápida, autohospedable, buenos SDK, modelo de token claro), mediasoup (más
control, hay que construir más), Janus o Jitsi (maduros, más pesados).

### 7.6 Fase 7 — Producto
- **Cliente web y móvil.** Ningún cliente IRC existente sirve para esto. Un SDK
  propio sobre el WebSocket que ya existe, hablando IRCv3 con las extensiones
  de las fases anteriores.
- **Notificaciones push** (APNs/FCM) desde un módulo, con *workers* para el
  HTTP saliente.
- **Búsqueda** sobre el historial (full-text de PostgreSQL primero; un motor
  aparte si hace falta).
- **Integraciones:** webhooks entrantes y salientes, bots — el API de `bot.c` y
  `Service{}` ya es la base correcta.
- **Administración:** consola, auditoría, retención, exportación,
  aprovisionamiento SCIM.

---

## 8. Orden, dependencias y esfuerzo

```
  F0 Cimientos (core, ABI 8) ─┬─► F1 Identidad ─┬─► F2 Historial ──► F3 Conversación
                              │                 │                       │
                              │                 └─► F5 Ficheros         │
                              │                                          ▼
                              └─► F4 Texto enriquecido            F7 Producto
                              │                                          ▲
                              └─► F6 Voz/vídeo/pantalla ─────────────────┘
```

- **F0 bloquea todo.** No hay atajo: con 7 bits de capacidad libres no caben
  las extensiones.
- **F6 (llamadas) es independiente de F2/F3.** Se puede hacer justo después de
  F0 y de la parte de autenticación de F1. Es el camino más corto a una demo
  con efecto.
- **F4 (texto enriquecido) sólo depende de F0.**

Órdenes de magnitud, como orientación y no como compromiso (equipo pequeño,
desarrollador con soltura en el código):

| Fase | Esfuerzo aproximado | Riesgo |
|---|---|---|
| F0 Cimientos | 2–3 meses-persona | Medio: toca el core, hay que no romper P10 |
| F1 Identidad | 2–4 | **Alto: decisión de producto, no técnica** |
| F2 Historial | 3–4 | Medio-alto: escala y retención |
| F3 Conversación | 2–3 | Bajo, una vez hay `msgid` |
| F4 Texto enriquecido | 1–2 | Medio: la degradación a texto plano |
| F5 Ficheros | 2–3 | Medio: es infraestructura nueva |
| F6 Voz/vídeo/pantalla | 3–5 | **Alto: WebRTC, NAT, escala de medios** |
| F7 Producto | Continuo | — |

### Camino corto recomendado (MVP demostrable)
**F0 → autenticación de F1 → F6.** Da un servidor en el que un equipo entra
con su cuenta, habla en canales y hace llamadas con vídeo y pantalla
compartida. Sin historial, que es la parte cara. Es la demo que justifica el
resto.

---

## 9. Riesgos y decisiones abiertas

1. **¿Vuelven las cuentas?** (§6.1) Condiciona F2, F3 y F7. Hay que decidirlo
   antes de F0.
2. **¿Workspaces por red o por namespace?** (§6.2) Condiciona el despliegue y
   el modelo de negocio.
3. **¿Se mantiene la compatibilidad con clientes IRC estándar?** Si es un
   requisito, toda extensión debe degradar limpiamente y nada puede ser
   obligatorio. Si no lo es, se puede ir mucho más rápido y con un protocolo
   más limpio. **Esta decisión afecta a cada fase.**
4. **Escala de P10.** El estado de canal se replica entero en cada servidor.
   Con workspaces grandes y muchos canales, es una carga que Slack resuelve de
   otra manera. Hay que medirlo antes de F2, no después.
5. **Un core de un solo hilo.** 002 ya dejó dicho el camino (los *workers*).
   Mientras la persistencia y el HTTP saliente vayan a *workers*, el modelo
   aguanta; la regla de que un worker no toca estado del core no se negocia.
6. **Los módulos no tienen sandbox.** Un módulo de historial con un fallo tumba
   la red. Cuanto más crítico el módulo, más severa la revisión y las pruebas.
7. **Cumplimiento normativo.** Historial + adjuntos + grabación de llamadas =
   datos personales. Retención, exportación, borrado y cifrado hay que
   diseñarlos en F2, no añadirlos en F7.

---

## 10. Lo que este documento recomienda no hacer

- **No** meter un servidor HTTP completo dentro del proceso del ircd.
- **No** hacer pasar medios por el ircd, en ninguna forma.
- **No** inventar un formato de texto enriquecido propietario cuando Markdown
  acotado más un tag de negociación resuelve lo mismo y es legible en cualquier
  cliente.
- **No** escribir en base de datos desde el hilo principal.
- **No** empezar por las funciones vistosas (reacciones, hilos) antes de F0: se
  reescriben enteras en cuanto exista `msgid`.
- **No** ampliar `ircd_parser.y` módulo a módulo: hacer el registro de bloques
  de configuración en F0 y pagarlo una sola vez.

---

## 11. Primer paso concreto

Si esta hoja de ruta se acepta, el primer cambio de código es pequeño, aislado
y verificable:

1. `capset_t` de 16 bits → mapa de bits, con la lista de capacidades en tiempo
   de ejecución (mismo patrón que la propuesta 003 usó para los modos de canal).
2. `module_add_cap()` en el API, ABI 8.
3. Una prueba unitaria del registro de capacidades y una prueba de integración
   que cargue un módulo con una capacidad propia, verifique `CAP LS`, `CAP NEW`
   y `CAP DEL`, y compruebe que la descarga del módulo la retira de todos los
   clientes conectados.

A partir de ahí, cada fase es una propuesta con su propio documento.
