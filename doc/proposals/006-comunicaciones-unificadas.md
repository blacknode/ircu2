# Propuesta 006 — De IRC a comunicaciones unificadas: texto enriquecido, voz, vídeo y pantalla compartida

**Estado:** hoja de ruta, aprobada con correcciones (revisión 3).
Fase 0 **en curso**: el registro de capacidades (§5.1), los hooks genéricos de
comando (§5.7), los identificadores de mensaje (§5.4) y `BATCH` con
`labeled-response` (§5.3), `draft/multiline` (§5.2) y la criptografía `ircd_*`
(§5.8) están implementados.
**Depende de:** 001 (API de módulos), 002 (hilos), 003 (modos por módulo),
004 (configuración desde el entorno), 005 (traducciones)
**Introduce:** `include/capab.h` + `ircd/capab.c`, los hooks de comando y
`include/msgid.h` + `ircd/msgid.c` (ya); el resto, por fases

**Correcciones sobre la revisión 2** (aprobación con cambios):
1. Una cuenta **es un nickname, sin excepción**, y un email agrupa **como
   máximo tres**, §6.2.
2. **MongoDB descartado.** El historial va sobre PostgreSQL y sólo sobre
   PostgreSQL, §7.1.
3. **WebRTC aplazado.** Voz, vídeo y pantalla compartida salen del roadmap
   activo y se retomarán más adelante, §7.5.

**Decisión tomada, y es la que más condiciona todo lo demás:** **se mantiene la
compatibilidad con los clientes IRC tradicionales.** Un cliente que no negocia
nada recibe exactamente las mismas líneas que recibía antes, byte a byte. Esto
deja de ser una pregunta abierta y pasa a ser la regla de aceptación de cada
cosa que se añada: toda extensión va detrás de una capacidad, ninguna es
obligatoria, y ninguna cambia lo que se le manda a quien no la pidió. Cuando
una extensión no pueda degradar limpiamente, el servidor genera el equivalente
en texto plano (§7.3); si ni eso es posible, la extensión no entra.

**Decisiones que vienen de la revisión 2 y siguen en pie:** hooks genéricos
de comando (§5.7), workspaces descartados (§6.3), SASL y `ACCOUNT` en el core
(§6.1), criptografía `ircd_*` en el core (§5.8), HTTP como módulo con registro
en el core (§7.4), clientes web y móvil en el roadmap (§7.7), aislamiento de
módulos como fase (§7.7).

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
- **Lo que no va dentro del proceso del ircd:** los medios. Un flujo de vídeo
  no pasa por P10 ni por el hilo del event loop. El ircd es **señalización y
  autorización**; el transporte de medios es un proceso aparte (§7.5) — propio,
  pero aparte.

---

## 2. Punto de partida: inventario de lo que ya hay

Esto no se empieza de cero. Lo ya construido cubre buena parte de los cimientos:

| Pieza | Dónde | Sirve para |
|---|---|---|
| API de módulos ABI 7 | `include/module.h` | Todo lo nuevo entra como módulo |
| Comandos desde módulo, con token P10 propio | `module_add_command()` | `RTC`, `HISTORY`, `REACT`… sin tocar el core |
| Modos de usuario y de canal desde módulo | 003, `chan_modes.c` | Estado de llamada, roles |
| Hooks de ciclo de vida (17 puntos, veto y reescritura) | `include/hooks.h` | Autorización, captura, filtrado |
| Hilos de trabajo | `worker.c`, `FEAT_WORKER_THREADS` | Hashing de contraseñas, HTTP saliente, colas |
| API de base de datos + driver PostgreSQL asíncrono | `db.c`, `modules/workers/postgres/` | El almacén de mensajes y de cuentas |
| Migraciones SQL versionadas por módulo | `migration.c`, `/MODULE MIGRATION` | Esquema del historial y de las cuentas |
| Bots y servicios en proceso | `bot.c`, `Service{}`, `irc_services` | Bots, integraciones, webhooks |
| WebSocket RFC6455 sobre los listeners | `websocket.c`, `Port { websocket = yes; }` | El cliente web se conecta ya |
| TLS (OpenSSL/GnuTLS/libtls) | `ircd_tls.h` | Transporte |
| Tags IRCv3 + `TAGMSG` + `CLIENTTAGDENY` | `msg_tag.c`, `m_tagmsg.c` | Reacciones, typing, metadatos por mensaje |
| Registro de mecanismos de cifrado | `ircd_crypt.c` (native/plain/smd5) | Base para Argon2 y bcrypt |
| SHA-1 y MD5 | `ircd_sha1.c`, `ircd_md5.c` | HMAC para credenciales TURN |
| i18n con ficheros PO, core y módulos | 005 | Producto multi-idioma |
| Host virtual obligatorio (`+x`, TEA) | `ircd_vhost.c` | Privacidad de IP por defecto |

Capacidades IRCv3 anunciadas hoy: `away-notify`, `chghost`, `echo-message`,
`invite-notify`, `userhost-in-names`, `message-tags`, `server-time`,
`draft/languages`, `cap-notify`.

---

## 3. Las brechas reales, medidas en el código

### 3.1 No hay memoria
Nada persiste un mensaje. No hay identificador de mensaje, ni historial, ni
marca de leído, ni no-leídos, ni búsqueda.

### 3.2 No hay identidad persistente
Las cuentas se retiraron (`doc/readme.accounting`): no hay nombre de cuenta, ni
SASL, ni `ACCOUNT`. Se revierte esta decisión; ver §6.

### 3.3 La línea es de 512 bytes
`BUFSIZE` es 512 (`include/ircd_defs.h`), `TOPICLEN` 160, `AWAYLEN` 160,
`NICKLEN` 15, `CHANNELLEN` 200. El buffer de reescritura de los hooks de
mensaje es `char rewrite[BUFSIZE]` (`ircd_relay.c:147`). Los tags sí tienen
8191 bytes (`TAGSLEN`), pero el cuerpo no.

### 3.4 `capset_t` es de 16 bits — el bloqueo duro (**resuelto**, §5.1)
`typedef unsigned short capset_t;` (`include/client.h:95`), y la lista de
capacidades es una macro-X en tiempo de compilación (`include/capab.h`). Hay
**9 capacidades usadas de 16: quedan 7 libres**, y un módulo no puede registrar
ninguna. Las que este producto necesita (`batch`, `labeled-response`,
`message-ids`, `sasl`, `draft/chathistory`, `draft/multiline`,
`draft/message-redaction`, `draft/react`, `draft/reply`, `draft/typing`,
`draft/read-marker`, `standard-replies`, `setname`, `metadata-2`,
`draft/filehost`) son quince.

### 3.5 Los hooks de mensaje sólo ven el origen local
`HOOK_MESSAGE_PRE_CHANNEL` y `HOOK_MESSAGE_PRE_PRIVATE` se disparan en
`relay_channel_message()` y `relay_private_message()` — mensajes de un cliente
*de este servidor*. Las rutas `server_relay_*` no los disparan. Un módulo de
historial sólo vería lo que se dijo en su propio servidor.

### 3.6 Los hooks son síncronos (**resuelto**, §5.5)
Todos los hooks corren en línea en el hilo principal, así que autenticar
contra una base de datos o verificar un token SSO desde uno bloquearía el
servidor entero. `HOOK_PENDING` ya deja suspender la operación y contestarla
más tarde, donde el core sabe volver a entrar. Ver §5.5.

### 3.7 No hay visibilidad sobre los comandos (**resuelto**, §5.7)
No existe ningún punto donde un módulo vea «este usuario va a hacer X sobre
este otro usuario». Auditoría, antiabuso, permisos por rol y registro de
acciones son hoy imposibles sin parchear cada `m_*.c`. Ver §5.7.

### 3.8 No hay HTTP
El WebSocket es sólo un *upgrade* que transporta líneas IRC.

### 3.9 Los módulos no tienen aislamiento
Un módulo con un fallo de memoria corrompe el estado del core y tumba el
servidor — y, por el efecto de un split, perturba la red. Ver §7.7.

---

## 4. Principio rector: qué va al core y qué va a un módulo

> **Al core va el mecanismo; al módulo va la política.**

Va al core lo que *no puede* estar fuera: el formato de la línea, el registro
de capacidades, el enrutado, los identificadores de mensaje, **todo lo que toca
`struct Client`**, la criptografía primitiva, y los puntos de extensión. Va a
un módulo lo que decide *qué* se hace con ello.

**Corolario que condiciona el diseño de todo lo demás:** los módulos se cargan
con `RTLD_LOCAL` y **no pueden resolver los símbolos de otro módulo**. Un
módulo nunca llama a otro directamente. Cuando un módulo tiene que ofrecer un
servicio a otros, el patrón es siempre el mismo, el que ya usa `include/db.h`:

- el **core** guarda un registro con un proveedor y una interfaz de tipos
  cerrados (`db_register_driver()`),
- el **proveedor** es un módulo que se registra al cargarse,
- el **consumidor** pregunta al core si hay proveedor (`db_available()`) y
  degrada limpiamente si no lo hay.

Esto aplica igual al HTTP (§7.4) y a cualquier servicio futuro entre módulos.

---

## 5. Fase 0 — Cimientos (core). Sin esto no hay nada

Todo lo de esta sección es trabajo de core y sube `IRCU_MODULE_ABI` a 8.

### 5.1 Capacidades dinámicas — **implementado**

Era el bloqueo duro y ya no lo es. Lo que hay en el árbol:

- `capset_t` pasa de `unsigned short` a un mapa de bits de **128 posiciones**
  (`DECLARE_FLAGSET(CapabSet, CAP_MAX)` en `client.h`, la misma maquinaria que
  `struct Privs`). `cli_capab()` y `cli_active()` devuelven un puntero, como
  `con_privs()`, de modo que los ~30 `CapHas(cli_active(x), CAP_Y)` del árbol
  no cambiaron ni una letra.
- `CAP_*` deja de ser una máscara y pasa a ser una **posición**. Donde una
  capacidad es opcional — los argumentos `require`/`forbid` de
  `sendcmdto_*_capab_*()` — se pasa `CAP_NONE`, nunca `0`: cero es ahora una
  posición válida (`away-notify`).
- **`ircd/capab.c`** es el registro en tiempo de ejecución, con la misma forma
  que los registros de modos de 003: `cap_first()`, `cap_find()`,
  `cap_find_index()`, `cap_register()`, `cap_unregister()`,
  `cap_drop_module()`. Ordenado por nombre, así que `CAP LS` sale igual se
  cargue el módulo cuando se cargue.
- **`ircd/m_cap.c`** queda como sólo el protocolo por encima: `CAP LS/REQ/
  ACK/LIST`, `cap_new()`, `cap_del()`. La separación es la de
  `migration.c` frente a `migration_run.c`, y es lo que permite probar el
  registro sin cliente ninguno (`capab_t`).
- **`module_add_cap()` / `module_del_cap()` / `module_cap_count()`** en el API,
  `IRCU_MODULE_ABI` a **8**. Al descargar el módulo se manda `CAP DEL` y se
  quita el bit a todos los clientes locales.
- La posición de una capacidad de módulo **se reparte, no se deriva del
  nombre**: una capacidad se negocia con un cliente y no cruza un enlace, así
  que no hay nada en que dos servidores tengan que coincidir. Es la diferencia
  con los modos de canal de 003, y está dicha en `doc/readme.modules`.

Pruebas: `capab_t` (registro: siembra del core en las posiciones que nombra
`enum Capab`, validación de nombres, reparto y devolución de posiciones,
agotamiento a `CAP_MAX`, los bitsets), `module_t` (registro desde módulo, borrado
explícito, reversión al descargar, 50 ciclos de carga/descarga) y comprobación
del protocolo contra un servidor real: `CAP LS`, `CAP LS 302` con valor,
`CAP REQ`/`ACK`, `CAP LIST`, negación y `NAK`.

De paso, un fallo que estaba ahí: `cap_new()` y `cap_del()` recorrían
`i < HighestFd`, saltándose el descriptor más alto — el resto del árbol usa
`i <= HighestFd`. Ese cliente no se enteraba de un `CAP NEW` ni perdía la
capacidad en un `CAP DEL`.

### 5.2 Longitud de línea — **implementado**

El cuerpo de una línea IRC son 512 bytes y no se pueden ampliar sin romper a
todo cliente existente. `draft/multiline` es la respuesta: el cliente abre un
batch, manda los trozos como PRIVMSG normales con `@batch=`, y lo cierra; el
servidor los junta y los relaya.

**La salida es lo que compra la compatibilidad.** Los trozos salen por el relay
de siempre, uno a uno: un cliente que no pidió nada ve la serie de mensajes
separados que ha visto siempre, y uno que sí la ve envuelta en un batch. No hay
una segunda ruta de entrega que mantener.

```
[C] BATCH +q draft/multiline #canal
[C] @batch=q PRIVMSG #canal :primera
[C] @batch=q;draft/multiline-concat PRIVMSG #canal : y su continuación
[C] BATCH -q

  cliente con multiline:
    @msgid=AB8ap... :ms!m@h BATCH +1 draft/multiline #canal
    @batch=1 :ms!m@h PRIVMSG #canal :primera y su continuación
    :ms!m@h BATCH -1
  cliente tradicional:
    :ms!m@h PRIVMSG #canal :primera y su continuación
```

**Un mensaje, un nombre.** El `msgid` va en la línea que abre el batch y los
trozos no llevan ninguno: son trozos, no mensajes, y darle un nombre a cada uno
haría que cualquier cosa que guarde mensajes archivara uno como varios. La
línea de cierre tampoco lo lleva: es un delimitador.

**Lo que no se vio venir y era lo que hacía inútil la función.** Cada línea
cuesta dos segundos de penalización de flood (`MFLG_SLOW`). Un cliente mandando
los 24 trozos que permite la especificación acumulaba 48 segundos de castigo y
el servidor dejaba de leerlo: mandar **un** mensaje te echaba. Un trozo de un
batch abierto ya no paga la penalización plana por comando — sólo los bytes,
que es lo que de verdad acota un cliente volcando datos, y `max-bytes` acota el
total. Con eso, 24 líneas a toda velocidad entran sin despeinar la conexión.

Límites como *features* (`MULTILINE_MAX_BYTES` 4096, `MULTILINE_MAX_LINES` 24),
anunciados en el valor de la capacidad para que el cliente sepa qué puede mandar
antes de mandarlo, y reanunciados en cada `/REHASH`.

**Pruebas:** `tests/multiline/` en la suite de integración, y comprobación
contra un servidor real: el mensaje de 24 líneas a toda velocidad sin que se
caiga el emisor, el `concat` que une sin salto, el cliente tradicional que ve
dos PRIVMSG limpios, el límite que se aplica con un 417, y el batch rechazado a
quien no pidió la capacidad.

### 5.3 `BATCH` y `labeled-response` — **implementado**

`BATCH` dice «estos mensajes van juntos» y es prerrequisito de `multiline`, de
`chathistory` y de cualquier entrega agrupada. `labeled-response` es lo que
permite a un cliente correlacionar petición y respuesta: sin eso una UI que no
sea un terminal no se puede escribir, porque no hay forma de saber qué
respuesta pertenece a qué comando.

**La decisión de diseño: no se almacena nada.** La especificación fija la forma
de la respuesta — nada enviado se contesta con un `ACK`, algo enviado va dentro
de un batch — y la lectura obvia de eso es que el servidor tiene que saber de
antemano cuántos mensajes va a producir el comando, es decir, almacenarlos y
contarlos. No hace falta: **el batch se abre de forma perezosa, en el primer
mensaje que el comando envía de verdad** (`label_before_send()`, llamado desde
`send_buffer()`). Si no llega ninguno, `label_end()` manda el `ACK` en su
lugar. Coste: una comparación por mensaje cuando no hay ninguna etiqueta en
vuelo, que es prácticamente siempre.

**Una etiqueta en vuelo a la vez**, y no es una simplificación sino la forma del
servidor: un comando se lee, se despacha, se maneja y se termina antes de mirar
la siguiente línea, así que una segunda etiqueta no puede empezar mientras la
primera está abierta. El estado es un contexto, no una tabla.

```
[C] @label=abc123 LUSERS
[S] @label=abc123 :irc.example.net BATCH +1 labeled-response
[S] @batch=1 :irc.example.net 251 lab :There are 1 users ...
[S] @batch=1 :irc.example.net 255 lab :I have 1 clients ...
[S] :irc.example.net BATCH -1

[C] @label=quiet PONG :nada
[S] @label=quiet ACK
```

La etiqueta va en la línea que abre el batch y en el `ACK`; no se repite en cada
mensaje ni en la línea de cierre, que ya queda identificada por el batch.

**Compatibilidad**, que es la regla del proyecto: un cliente que no negoció nada
recibe las líneas de siempre, y su `@label=` se descarta con el resto de tags de
servidor que un cliente no puede fijar. Un cliente que pidió `labeled-response`
pero no `batch` **se contesta como antes, no a medias**: la especificación
construye una sobre otra y media respuesta no la sabe leer ningún cliente.

**Dos cosas que salieron de implementarlo:**

1. `msg_tag_filter_client()` descartaba todo tag de cliente que no fuera `+`, y
   `label` con él — la línea etiquetada ni siquiera llegaba al despacho. Es la
   política correcta (un cliente no puede fijar un tag de servidor) y `label` es
   la única excepción legítima: es el cliente nombrando su propia petición, y no
   va más allá del comando en que llegó.
2. `MSG_BATCH` choca con el flag de socket de glibc (`<bits/socket.h>`), que lo
   define como miembro de un `enum`: la macro se expandía dentro de él y el
   fichero dejaba de compilar. La macro se llama `MSG_IRCBATCH`; el comando en
   el cable sigue siendo `BATCH`.

**Pruebas:** `batch_t` (el `ACK` cuando no se envía nada, la apertura perezosa,
la etiqueta sólo en la línea de apertura, un batch pertenece a un cliente, las
dos capacidades hacen falta, el cliente que se va a mitad de respuesta) y
`tests/labeled/` para la suite de integración. Comprobado contra un servidor
real en las tres ramas de la especificación.

### 5.4 Identificadores de mensaje (`msgid`) — **implementado**

Un nombre para un mensaje, el mismo en toda la red. Es la clave primaria de las
cinco cosas que vienen después — historial, hilos, reacciones, ediciones y
marcas de leído —, y ninguna de ellas se puede construir sin él.

**Lo que hay en el árbol:**

- **`ircd/msgid.c`** es sólo el generador: el numérico P10 del servidor más un
  contador en base 62. El numérico separa los identificadores de este servidor
  de los de cualquier otro sin negociar nada — dos servidores no pueden
  compartir numérico —, y el contador se siembra del reloj, de modo que un
  reinicio nunca vuelve a repartir identificadores que ya dio. No sabe nada de
  clientes ni de envíos, que es lo que permite probar aparte la única propiedad
  que importa. Salen de unos doce caracteres: `AB8aoSr25JN`.

- **El identificador es de la línea, no del envío.** Una línea que entra es un
  mensaje, se convierta en los envíos que se convierta: el reparto al canal, el
  eco al remitente y la copia que cruza cada enlace tienen que llevar el mismo,
  o nada río abajo puede saber que son el mismo mensaje. `parse_dispatch()`
  abre y cierra la línea alrededor del handler
  (`msg_tag_line_begin()`/`msg_tag_line_end()`), y fuera de esa ventana no hay
  identificador ninguno — que es la respuesta correcta: un mensaje que el
  servidor se inventa no es de un usuario y nada va a querer referirse a él.

- **Sólo lo lleva el comando de la propia línea.** `msg_tag_line_msgid(tok)`
  devuelve el identificador únicamente si `tok` es el comando que la línea
  traía. Salió de una comprobación contra un servidor real: sin eso, un numeric
  emitido mientras se maneja un `PRIVMSG` — un `403 No such channel` — heredaba
  el nombre del mensaje, y cualquier cosa que guardara mensajes habría archivado
  el error bajo el nombre del mensaje.

- **Alcance: `PRIVMSG`, `NOTICE` y `TAGMSG`.** `WALLCHOPS` y `WALLVOICES` se
  dejaron fuera a propósito: salen al canal como `WALLCHOPS` pero se le
  devuelven a su remitente como `NOTICE`, así que el remitente tendría un
  nombre distinto del que tiene todo el mundo — y un mensaje sobre cuyo nombre
  dos clientes no se ponen de acuerdo es peor que uno sin nombre.

- **Un cliente nunca elige el suyo.** Un `@msgid=` que llega de un cliente se
  descarta y se genera uno nuevo: un identificador que un cliente pudiera
  elegir es uno con el que podría apuntar a — o sobrescribir — el mensaje de
  otro en lo que sea que los guarde. El que llega de un servidor sí se conserva
  y se reenvía, y eso es lo que hace que toda la red llame igual al mismo
  mensaje.

- **Compatibilidad, que es la regla nueva (§ cabecera).** El identificador sale
  hacia un cliente sólo si negoció `message-tags`, y hacia otro servidor sólo
  bajo `FEAT_NETWORK_FEATURES`. Un cliente tradicional recibe la línea de
  siempre, byte a byte.

**Comprobado**, además de `msgid_t` (20.000 identificadores sin repetir, el
reinicio que no retrocede, dos servidores que no colisionan, la validación de
lo que llega de un par): contra un servidor real y contra un enlace P10 de
verdad, con el servidor falso de `tests/p10_server.py`.

```
  cliente tradicional : :snd4!snd4@... PRIVMSG #q4 :uno
  cliente con tags    : @time=...;msgid=AB8aoSr25JN :snd4!... PRIVMSG #q4 :uno
  la línea S2S        : @time=...;msgid=AB8aoSr25JN ABAAF P #q4 :uno
```

El mismo mensaje tiene un solo nombre a los dos lados del enlace, y el cliente
que no pidió nada no ve nada.

### 5.5 Hooks asíncronos (`HOOK_PENDING`) — implementado
Un hook devuelve `HOOK_PENDING`, la operación queda suspendida y el módulo la
reanuda con `hook_resume(token, HOOK_ALLOW|HOOK_DENY, motivo)` cuando su
consulta termina. Es lo que hace posible autenticar contra un almacén externo
sin parar el servidor — y, más adelante, lo que hace viable el aislamiento de
módulos (§7.7), porque un módulo fuera de proceso responde por fuerza de forma
asíncrona.

Cuatro decisiones, y la razón de cada una:

- **Se suspende donde el core sabe volver a entrar, y el módulo lo ve.**
  `hc_token` en el contexto vale distinto de cero exactamente en esos puntos;
  en cualquier otro, un `HOOK_PENDING` se anota en el log y se lee como
  `HOOK_CONTINUE`. Un módulo que no mira `hc_token` es un módulo cuya política
  no se aplica, pero no en silencio.

- **El único punto suspendible hoy es `HOOK_CLIENT_PRE_REGISTER`**, y por eso
  se disparó desde `auth_module_check()` en `s_auth.c` y no desde
  `register_user()`. El registro ya es una máquina de estados que espera
  —ident, DNS, CAP, la *cookie* de PING, iauth—, así que la retención de un
  módulo es una bandera más al lado de esas (`AR_MODULE_PENDING`,
  `AR_MODULE_CHECKED`). Dentro de `register_user()` no habría a dónde volver.

- **Suspender corta la cadena**, igual que `HOOK_ALLOW` y `HOOK_DENY`: el
  módulo que suspendió se queda con la decisión. Mantener un iterador vivo
  durante la espera sería guardar punteros a una cadena que una descarga de
  módulo puede reescribir entre medias.

- **Expirar es denegar.** `FEAT_HOOK_TIMEOUT` (10 s por defecto) pone el
  plazo, y descargar el módulo mientras debe una respuesta acaba igual. Un
  módulo que suspende un punto de veto está decidiendo si algo puede ocurrir;
  dejar pasar la operación porque nunca contestó sería exactamente el
  resultado que se le pidió evitar. El plazo lo vigila un temporizador propio
  de `hooks.c` armado sobre el vencimiento más próximo: colgarlo de
  `check_pings()` no servía, porque esa pasada se programa con minutos de
  antelación en un servidor ocioso y un plazo que solo se cumple *a veces* no
  es un plazo.

Mientras un registro está retenido el nick queda congelado (`m_nick.c`
responde 437): la pregunta que se le hizo al módulo era sobre esa conexión con
ese nombre. Y si el cliente se va, la retención se descarta sin llamar a
nadie — no hay a quién denegarle nada, y el *callback* no debe correr sobre un
`struct Client` que se está liberando.

`modules/hooks/slowauth.c` es el módulo de referencia: retiene el registro,
pregunta en un *worker* y reanuda. Con ocho clientes conectando a la vez
contra dos hilos de *worker* y 250 ms de consulta, el servidor los atiende a
todos en paralelo; con la consulta en el hilo principal serían dos segundos
con el servidor parado.

### 5.6 Puntos de extensión que faltan
- `module_add_isupport()` — tokens en el 005 desde un módulo.
- `module_add_feature()` — *features* propias, visibles en `/GET` y `/SET`.
- `module_add_config_block()` — bloques de configuración propios, en vez de
  ampliar `ircd_parser.y` por cada módulo.
- `module_add_numeric()` — numerics propios, con su contexto de traducción.
- Hooks nuevos: `HOOK_MESSAGE_DELIVERED` (incluido el origen remoto, §3.5),
  `HOOK_CHANNEL_TOPIC_CHANGED`, `HOOK_CLIENT_AWAY`, `HOOK_PRESENCE_CHANGED`.

### 5.7 Hooks genéricos de comando — **implementado**

**El problema.** No había forma de que un módulo observara o vetara una acción
de un usuario sobre otro: `KICK`, `KILL`, `WHOIS`, `GLINE`, `SLINE`, `JUPE`,
`MODE`, `INVITE`, `SILENCE`. Un hook por comando no escala: son decenas, y cada
comando nuevo — del core o de un módulo — necesitaría el suyo.

**Lo que hay en el árbol:**

- **Dos puntos, `HOOK_COMMAND_PRE` y `HOOK_COMMAND_POST`**, instrumentados en
  los dos únicos despachos que tiene `ircd/parse.c` — uno en `parse_client()` y
  otro en `parse_server()` —, ambos reunidos en `parse_dispatch()` para que las
  reglas estén escritas una sola vez. Ningún `m_*.c` se tocó.

- **`struct HookCommand`** en `ctx->hc_command`: nombre, token P10,
  `HandlerType`, `parc`, `parv` y, en `POST`, lo que devolvió el handler.

- **El sujeto lo declara cada comando**, en `msgtab[]`, con un descriptor en
  `struct Message`:

  ```c
  struct MsgSubject {
    unsigned char ms_target;   /* parv[] con un nick o numnick */
    unsigned char ms_channel;  /* parv[] con un canal */
    unsigned char ms_mask;     /* parv[] con una máscara user@host */
    unsigned char ms_reason;   /* parv[] con el texto libre */
  };
  ```

  Tres detalles que salieron de escribirlo y no del diseño sobre papel:

  1. **El índice cero significa «no aplica»**, no `-1`. `parv[0]` es el origen,
     nunca un sujeto, así que un comando que no declara nada acierta por
     omisión — y `msgtab[]` se inicializa por posición, de modo que las ~200
     entradas que no declaran sujeto no se tocaron.
  2. **`MS_LAST`** para un objetivo cuya posición se mueve con el número de
     parámetros. `WHOIS` es `WHOIS <nick>` y `WHOIS <servidor> <nick>`: en
     ambos el nick es el último. Un índice fijo habría acertado la mitad de las
     veces, que es peor que no decir nada.
  3. **`subject_s`**, un segundo descriptor para cuando el comando no tiene la
     misma forma en los dos lados. Un `GLINE` de un oper empieza por la
     máscara; uno de un servidor empieza por el servidor al que va dirigido y
     lleva la máscara una posición más allá. Todo a cero significa «igual que
     la forma de cliente», que es el caso normal.

  | Comando | target | channel | mask | reason |
  |---|---|---|---|---|
  | `KICK`    | 2 | 1 | — | 3 |
  | `KILL`    | 1 | — | — | 2 |
  | `WHOIS`   | `MS_LAST` | — | — | — |
  | `INVITE`  | 1 | 2 | — | — |
  | `MODE`    | 1 | 1 | — | — |
  | `SILENCE` | — / 1 | — | 1 / 2 | — |
  | `GLINE`   | — | — | 1 / 2 | — |
  | `JUPE`    | — | — | 1 / 2 | — |
  | `SLINE`   | — | — | — / 5 | — |

  (donde hay dos valores, el primero es la forma de cliente y el segundo la de
  servidor.)

- **La resolución vive en `parse.c`, no en `hooks.c`.** Fue una corrección
  durante la implementación: poner la búsqueda en `hooks.c` acoplaba el
  despachador a `hash.c` y `numnicks.c` y rompía la prueba unitaria de los
  hooks, que no enlaza el servidor. `parse.c` es dueña de `msgtab[]`, ya incluye
  las dos, y es el sitio natural. `hooks.c` sigue siendo un despachador puro.
  Y esa resolución es lo que se está comprando: **qué búsqueda usar depende de
  por dónde llegó la línea, no del comando** — el mismo `KICK` lleva un nick
  desde un cliente y un numnick desde un servidor, que es la distinción que
  hace cada `ms_*()` del árbol y que ningún módulo debería tener que repetir.

- **Registro por comando**: `module_add_command_hook(mod, tipo, "KICK", fn,
  prioridad, user, flags)`. `hook_add()` rechaza los tipos de comando y
  `hook_add_command()` rechaza los de ciclo de vida: registrar un hook de
  comando sin nombrar comando es recibir cada línea que el servidor parsea,
  tráfico entre servidores incluido, para tirar casi todo. Se puede hacer
  pasando `NULL`, pero hay que quererlo.

- **Coste.** `hook_command_active()` delante de todo: una lectura y una
  comparación por línea cuando ningún módulo escucha, que es el caso normal.
  Cuando alguno escucha, se recorre una cadena corta comparando el nombre. El
  contador por comando que proponía la revisión 2 no se implementó: en el
  camino caliente, donde ya hay una búsqueda en árbol, varias `feature_bool()`
  y una reserva de `msgq`, un par de `ircd_strcmp` no se miden.

**Las cinco reglas, tal como quedaron:**

1. **Origen `+S` excluido** de los dos puntos, salvo que el hook pase
   `HOOK_CMD_INCLUDE_SERVICES`.
2. **El veto sólo vale en local.** `HOOK_DENY` en `PRE` se honra únicamente si
   `MyConnect(sptr)`. Para un comando llegado de otro servidor el punto es sólo
   notificación y el intento se registra en el log: el resto de la red ya lo
   aplicó, y rechazarlo aquí desincroniza este servidor en vez de impedir nada.
3. **`POST` no corre tras `CPTR_KILLED`** — los punteros serían memoria
   liberada — ni tras un veto: no ha pasado nada que contar.
4. **`parv` de sólo lectura.** Lo que un módulo puede cambiar es el contexto: un
   numeric y una razón al denegar.
5. **Recursión acotada** a ocho niveles, con registro en el log.

**Pruebas.** `hooks_t` cubre las cinco reglas (filtro por comando, exclusión de
`+S`, veto local frente a remoto, la recursión que se detiene sola, y que los
dos registros se rechazan mutuamente), `module_t` el registro desde módulo y su
reversión al descargar, y `modules/hooks/cmdaudit.c` es el módulo de
referencia, comprobado contra un servidor real: registra en el log
`KICK: ann -> bob on #room: behave` con el sujeto ya resuelto, y rechaza un
`KICK` contra un operador con `482 ann KICK :that user is an operator` — sin
que el `POST` llegue a registrar nada de ese comando rechazado.

### 5.8 Criptografía en el core — **implementado**

| Fichero | Para qué |
|---|---|
| `ircd/ircd_sha256.c` | SHA-256 y HMAC-SHA-256: firmar lo que el servidor reparte y tiene que reconocer después |
| `ircd/ircd_aes.c` | AES-256 **sólo en GCM** |
| `ircd/ircd_argon2.c` | BLAKE2b y Argon2id: lo que se guarda de una contraseña |
| `ircd/ircd_pwhash.c` | La forma almacenable, con sus costes dentro |

**No dependen del backend TLS.** `IRCU_TLS` puede valer `none` y puede ser
GnuTLS o libtls, así que echar mano de la biblioteca que haya enlazada
funcionaría en una compilación y no compilaría en la siguiente.

**Sólo GCM, a propósito.** No hay interfaz para cifrar un bloque sin
autenticarlo. Todos los usos que este servidor tiene — sellar un token que
volverá a leer, guardar un secreto de segundo factor — son casos en los que un
atacante que pueda cambiar el criptograma sin que se note ha roto la cosa
entera. Un cifrado sin etiqueta es un cifrado cuya salida puede editar
cualquiera.

**Los costes viajan con el hash** (`$argon2id$v=19$m=…,t=…,p=…$sal$tag`) y no
se leen de la configuración al verificar: eso es lo que permite subirlos más
adelante sin invalidar todas las contraseñas ya guardadas.
`ircd_pwhash_outdated()` dice cuándo conviene rehacer una. Y admite una *pepper*
de servidor, de modo que una base de datos robada no basta para empezar a
adivinar: hace falta también la configuración.

**Cómo se sabe que las constantes están bien.** Una tabla mal copiada compila,
corre y produce una salida perfectamente consistente: la única cosa capaz de
distinguir un dígito bueno de uno malo es un vector publicado. Así que la caja
de sustitución de AES **se calcula** a partir de su definición en GF(2^8) en vez
de escribirse, y todo lo demás está cubierto por FIPS 180-4, RFC 4231,
FIPS 197 C.3, los vectores del propio GCM, RFC 7693 y RFC 9106, en `crypto_t`.

Y sirvieron: el vector de Argon2id falló dos veces antes de pasar. La primera
porque el de la RFC incluye *secret* y *associated data* que el API no tomaba —
lo que a su vez añadió la *pepper*, que era útil de verdad. La segunda por dos
errores reales: la permutación del bloque operaba sobre dos grupos de ocho
palabras en vez de sobre las dieciséis que pide la especificación, y el bloque
de direcciones no llegaba a generarse para el primer segmento, que empieza en el
índice dos y por tanto nunca disparaba la condición del bucle. Ninguno de los
dos habría dado la cara sin el vector.

Aparte, ASan encontró un desbordamiento de pila en el parseo: un `%127s` de
`sscanf` escribiendo en un buffer de 64 bytes. Los dos campos base64 van ahora
acotados y con una comprobación en tiempo de compilación de que el ancho cabe.

**El hashing no corre nunca en el hilo principal.** Argon2 tarda entre 50 y 250
ms y reserva decenas de megabytes *a propósito*: diez inicios de sesión
simultáneos pararían el servidor un segundo. `ircd_pwhash_make()` y
`_verify()` son **puras** — no leen estado del core, no reservan nada que
sobreviva a la llamada — precisamente para poder ir por `worker_submit()`.

**No hay bcrypt**, y conviene decir por qué: son 1042 constantes que habría que
transcribir, su único uso aquí sería importar hashes de un sistema que todavía
no existe, y cuando haga falta lo correcto es traerse el `crypt_blowfish` de
Openwall, no volver a teclear las tablas. Argon2id es el algoritmo recomendado y
es el que se usa.

**Criterio de aceptación de la fase 0:** un módulo de ejemplo registra una
capacidad, un token ISUPPORT, una *feature*, un hook de comando y un hook
asíncrono, y `/MODULE UNLOAD` lo revierte todo sin dejar rastro en ningún
cliente conectado. Vectores de prueba oficiales (NIST/RFC) para los cuatro
algoritmos, en pruebas unitarias.

---

## 6. Fase 1 — Identidad

### 6.1 SASL y `ACCOUNT` vuelven, y van al core

**Decisión tomada.** Se revierte la retirada documentada en
`doc/readme.accounting`, y se revierte **sin la filosofía de Undernet**: la
cuenta no es «el nick al que te identificaste», es una identidad de primera
clase con vida propia. Van al core porque tocan `struct Client`.

Lo que entra en el core:

- `ircd/m_account.c` — comando `ACCOUNT`, token P10 `AC` (libre; los tokens
  ocupados están en `include/msg.h`). Autentica **antes o después** de
  completar la conexión:
  - antes: durante el registro, como `PASS`, para quien no use SASL;
  - después: identificarse más tarde, cambiar de cuenta activa, cerrar sesión.
- `ircd/m_authenticate.c` + capacidad `sasl` — SASL propiamente dicho, con
  `PLAIN` y `EXTERNAL` (certificado de cliente) en el core.
- Un **registro de mecanismos SASL** para que un módulo añada los suyos
  (`SCRAM-SHA-256`, `OAUTHBEARER` para SSO/OIDC) sin tocar el core.
- Campos de cuenta en `struct Client`, el tag `account`, y la propagación en
  P10 y en el burst.

Lo que **no** entra en el core: la comprobación de la credencial. Eso es
política, vive en un módulo `identity` con sus propias migraciones, y se
resuelve por el hook asíncrono de §5.5 más el hash en un worker (§5.8). El core
habla el protocolo; el módulo decide si la contraseña es correcta.

### 6.2 El modelo: un email, hasta tres cuentas, y una cuenta es un nick

**Decisión tomada.** La credencial es el **email**; una cuenta **es un
nickname, sin excepción**; un email agrupa **como máximo tres cuentas**.

```
  identity (email)                    account
  ────────────────                    ───────
  email          ◄── credencial       nick        ◄── ES el nickname
  password_hash      (argon2)         identity_id
  mfa_secret         (aes-256-gcm)    created_at
  sso_subject                         is_default
  verified_at                         suspended_at
  created_at
       │                                   ▲
       └──────────── 1 : N ────────────────┘
                   N <= 3
```

Que la cuenta **sea** el nick, y no un nombre aparte, quita de en medio la
pregunta que más complica un sistema de cuentas sobre IRC — «¿qué relación hay
entre el nick que uso y la cuenta a la que estoy identificado?». Aquí no hay
relación que mantener: son lo mismo. Lo que se sigue de ahí:

- **Registrar una cuenta es registrar un nick.** El nombre de la cuenta pasa
  las mismas comprobaciones que un nick (`NICKLEN`, el juego de caracteres de
  `ircd_chattr`, la comparación insensible a mayúsculas de `ircd_strcmp`) y
  colisiona con los nicks en uso exactamente igual.
- **`+r` recupera su significado literal.** Hoy `+r` significa «identificado al
  nick que uso» (`doc/readme.accounting`), y con este modelo eso sigue siendo
  cierto sin excepciones: si estás identificado, el nick que llevas es tu
  cuenta. Lo que cambia no es el significado de `+r`, es quién puede
  concederlo y cómo se prueba.
- **Cambiar de nick es cambiar de cuenta, o dejar de estar identificado.** Es
  la regla que hay que decidir explícitamente y documentar; la actual
  (`doc/readme.accounting`: un cambio de nick limpia `+r` en todos los
  servidores) sigue siendo la correcta y no hay que tocar nada.
- **El límite de tres es del email, no del servidor.** Se comprueba al
  registrar, en el módulo `identity`, y el número es configuración.

**Consecuencia que conviene ver ahora y no al implementar:** una persona no
tiene un nombre de visualización libre. Su nombre visible es un nick de
`NICKLEN` caracteres, único en la red. Un producto tipo Slack normalmente
enseña «María García» junto a un `@handle`; aquí el `@handle` es la identidad y
el nombre bonito hay que llevarlo aparte, por `setname` o `metadata-2`, sin que
nada dependa de él. Es coherente con haber descartado los workspaces (§6.3):
mientras el espacio de nombres sea plano y global, el nick es la identidad.

Lo demás del modelo no cambia:

- **La recuperación, la verificación, el 2FA y el SSO cuelgan del email**, no
  de la cuenta. Se implementan una vez para las tres.
- **Al autenticar hay que elegir cuál de las tres.** `ACCOUNT LOGIN <email>
  <password> [<nick>]`; sin el tercer parámetro se usa la marcada por defecto.
  En SASL el mismo dato viaja en el `authzid`, que es exactamente para lo que
  ese campo existe.
- **El email no se publica nunca.** No aparece en `WHOIS`, ni en ningún
  numeric, ni cruza P10. Lo que viaja por la red es el nick. Es requisito de
  privacidad y hay que imponerlo en el código y verificarlo con pruebas, no
  confiarlo al criterio de quien escriba el siguiente módulo.

### 6.3 Workspaces y namespaces: descartado por ahora

**Decisión tomada: fuera del roadmap.** Aislar espacios de trabajo dentro de
una red IRC no es una funcionalidad más: rompe supuestos que están en todas
partes — el espacio global de nicks de 15 caracteres, la visibilidad de canales
en `LIST`, `WHOIS` y `WHO`, el modelo de estado replicado de P10. Hacerlo a
medias produce un aislamiento que parece funcionar y filtra información.

Lo que corresponde es diseñarlo aparte, con su propia propuesta, asumiendo que
probablemente implica un cambio de protocolo de fondo — lo que llamaríamos
IRCv4 — y no un parche sobre P10. Hasta entonces: **una red, un espacio.**

Consecuencia práctica: el producto que sale de este roadmap sirve a **una
organización por despliegue**. Es una limitación real y hay que decirla en voz
alta, porque condiciona el modelo de negocio.

---

## 7. Fases 2 a 7 — El producto

### 7.1 Fase 2 — Historial, sobre PostgreSQL

**Decisión tomada: MongoDB descartado.** El historial va sobre PostgreSQL y
sólo sobre PostgreSQL. Queda anotado por qué, para no volver a discutirlo
dentro de un año:

- **La complejidad no distingue a los dos motores.** Los índices de MongoDB son
  B-tree, igual que los de PostgreSQL: una búsqueda por igualdad es O(log n) en
  ambos. Los índices `hashed` de Mongo sí son de dispersión, pero sólo sirven
  para igualdad exacta — no para rangos ni para ordenar — y su propósito real
  es repartir *shards*. El acceso del historial es intrínsecamente un rango
  ordenado (`CHATHISTORY BEFORE <msgid> LIMIT 50`), así que un índice de
  dispersión no es aplicable en ninguno de los dos. La búsqueda de texto es un
  índice invertido en ambos y tampoco es O(1) en ninguno.
- **Lo que Mongo aportaría es sharding de escritura, TTL y change streams.** Lo
  que PostgreSQL aporta y ya está aquí es particionado declarativo por tiempo,
  índices BRIN — minúsculos y hechos para datos ordenados en el tiempo —,
  búsqueda de texto integrada (`tsvector` + GIN), y la misma base que las
  cuentas, con lo que un borrado por usuario es una transacción y no una
  coreografía entre dos motores.
- **El coste no es escribir el driver, es `include/db.h`.** Está diseñado sobre
  SQL: `struct DbQuery` es texto con marcadores `$n`, `struct DbParam` son
  parámetros posicionales, y una migración es un *script* SQL con `up`/`down`
  dentro de una transacción. Un driver de Mongo obliga a generalizar todo eso a
  una segunda forma, con el riesgo de dejar la abstracción en el mínimo común
  denominador de dos motores que no se parecen.

**El diseño, entonces:**

- Tabla **particionada por mes**, clave primaria `msgid`, índice **BRIN** sobre
  el tiempo, **GIN** sobre el texto para la búsqueda.
- El módulo `history` define **su propia interfaz interna de almacén** — la
  forma de la consulta (`latest`, `before`, `after`, `around`, `between`,
  `targets`) se expresa ahí, no en SQL suelto repartido por el código. No es
  por dejar la puerta abierta a otro motor: es que el código de la sala no
  tiene por qué saber SQL, y una interfaz explícita es lo que hace que la
  retención y el borrado tengan un único sitio donde ocurrir.
- Captura en `HOOK_MESSAGE_DELIVERED` (§5.6), no en el hook de origen local,
  que no ve lo que llega de otros servidores (§3.5).
- Escrituras por el driver asíncrono; **nunca** en el hilo del event loop.
- `CHATHISTORY LATEST|BEFORE|AFTER|AROUND|BETWEEN|TARGETS`, dentro de un
  `BATCH`.
- Retención, purga, exportación y **borrado por cuenta**: requisito legal, se
  diseña aquí y no se añade al final.
- **Topología:** cada servidor escribe lo que entrega y la deduplicación por
  `msgid` la hace la base de datos. Escala mejor que un archivador designado.

### 7.2 Fase 3 — Semántica de conversación moderna

Todo esto son tags sobre `msgid`, y cabe en módulos una vez existe la fase 0:

- **Hilos:** tag `+draft/reply=<msgid>`.
- **Reacciones:** `TAGMSG` con `+draft/react`, persistidas por `history`.
- **Edición y borrado:** `draft/message-redaction` (`REDACT`), con política de
  quién y durante cuánto tiempo.
- **Typing:** `+typing`, efímero, sin persistir.
- **Marcas de leído y no-leídos:** `draft/read-marker` (`MARKREAD`), por cuenta
  y por objetivo. Es lo que convierte esto en una bandeja de entrada.
- **Menciones:** derivadas de la cuenta, no del nick.

### 7.3 Fase 4 — Texto enriquecido

IRC no tiene tipo de contenido. **No inventar un dialecto de códigos de
control.** Negociar el formato con una capacidad propia (`blacknode/richtext`)
y llevar el cuerpo como Markdown acotado (negrita, cursiva, tachado, código,
bloque, cita, lista, enlace, mención), señalizado con `+blacknode/format=markdown`.

- Cliente sin la capacidad → recibe el texto plano equivalente, generado por el
  servidor. Innegociable: sin esto la red se parte en dos.
- El saneado (nada de HTML, límites de anidamiento y de longitud) se hace en el
  servidor, en un hook de reescritura. Nunca en el cliente.
- Bloques largos → `draft/multiline` de la fase 0.

### 7.4 Fase 5 — HTTP como módulo, y ficheros

**Decisión tomada: HTTP es un módulo**, y un módulo que quiera usarlo debe
comprobar que está disponible.

Por §4, un módulo no puede llamar a otro. La forma correcta es la misma que la
de la base de datos, y va en el core como interfaz delgada:

```c
/* include/http.h -- el core sólo guarda el registro y despacha. */
extern int  http_register_provider(struct ModuleHandle* mod,
                                   const struct HttpProvider* provider);
extern void http_unregister_provider(struct ModuleHandle* mod);

/** ¿Hay proveedor cargado?  Un consumidor comprueba esto y degrada si no. */
extern int  http_available(void);
extern const char* http_provider_name(void);

/** Registrar una ruta.  Se revierte al descargar el módulo consumidor. */
extern int  http_add_route(struct ModuleHandle* mod, const char* method,
                           const char* path, HttpHandlerFn fn, void* user);
```

- El **proveedor** es un módulo (`modules/workers/http/`) que posee el socket,
  el parseo y su TLS, y que hace el trabajo de red **en su propio worker**,
  entregando al hilo principal sólo peticiones ya parseadas.
- El **consumidor** hace `if (!http_available()) { /* función desactivada */ }`
  al cargarse y lo dice en el log y en `/MODULE LIST`. No se cae, no adivina:
  se desactiva esa parte de su funcionalidad.
- Las rutas se revierten al descargar cualquiera de los dos módulos.

Sobre esto se construyen los ficheros:

1. **Subida y descarga** con almacenamiento de objetos detrás (S3 o compatible).
2. **Autorización desde el ircd:** token de vida corta sellado con AES-256-GCM
   o firmado con HMAC-SHA256 (§5.8), ligado a la cuenta y al canal; el
   proveedor HTTP lo valida. El mensaje resultante lleva la URL y los metadatos
   en tags (`draft/filehost`).
3. Miniaturas y antivirus, en *workers*.

Lo mismo vale para webhooks entrantes y para una API REST de integraciones: son
rutas sobre el mismo proveedor.

### 7.5 Voz, vídeo y pantalla compartida — **aplazado**

**Decisión tomada: fuera del roadmap activo.** WebRTC se retomará más adelante,
con su propia propuesta. Lo que sigue es lo que hay que conservar de la
discusión para que retomarlo no sea empezar de cero.

**Por qué aplazarlo es razonable.** Es la única parte del producto que no
depende de ninguna otra y de la que ninguna otra depende: la señalización se
apoya en el estado de canal, que ya existe, y nada de lo que se construya en
las fases 2 a 7 cambia por haberlo pospuesto. Aplazarlo no crea deuda; hacerlo
antes que el historial y la identidad, sí habría desplazado esfuerzo desde lo
que sostiene el producto hacia lo que lo enseña.

**Lo que quedó decidido y sigue valiendo cuando se retome:**

- **El ircd es señalización y autorización, nunca medios.** Un módulo `rtc` con
  comando y token P10 propios (`RTC START|JOIN|LEAVE|OFFER|ANSWER|CANDIDATE|
  END`). Quién puede iniciar una llamada en un canal y quién puede unirse sale
  de la pertenencia y de los modos: el estado que el ircd ya tiene y que nadie
  más tiene. Ésa es la razón de que la señalización viva aquí y no fuera.
- **Implementación propia, y se descartaron los SFU de terceros.** Implementación
  propia quiere decir que **el router es nuestro**, no que reimplementemos los
  transportes: ICE (`libjuice`/`libnice`), DTLS (OpenSSL), SRTP (`libsrtp2`),
  SCTP (`usrsctp`). Lo que se escribe es lo diferencial — router de paquetes,
  selección de capa en simulcast, estimación de ancho de banda, keyframes,
  salas. Y el SFU es un **proceso aparte**: un fallo en el camino de medios no
  puede tumbar la red.
- **Lo primero que se haría al retomarlo no necesita SFU.** Una llamada de 1 a
  1, y un grupo pequeño en malla, conectan a los pares directamente. Con el
  módulo de señalización y un TURN ya hay voz, vídeo **y pantalla compartida**
  entre dos personas y en grupos de tres o cuatro. El SFU es el salto que hace
  falta a partir de ahí, no el punto de partida.
- **Pantalla compartida no es una función aparte:** es una pista de vídeo más
  (`getDisplayMedia`) con su etiqueta. Si la llamada funciona, compartirla es
  trabajo de cliente.
- **TURN sigue siendo coturn.** Un TURN es un relé UDP autenticado, está
  resuelto desde hace quince años, y escribir uno propio no aporta nada al
  producto. Las credenciales REST efímeras se emiten desde el ircd con HMAC
  (`ircd_sha1.c`, o el `ircd_sha256.c` de §5.8).
- **El riesgo que motivó aplazarlo:** un SFU completo es un producto por
  derecho propio, del orden de 12 a 24 meses-persona. Retomarlo será una
  decisión con usuarios reales delante, no una estimación.

**Lo que esto cambia en el resto del roadmap:** nada, salvo que §5.8 pierde una
de sus justificaciones — el sellado de tokens de sala. Las otras tres (hash de
contraseñas, 2FA, tokens de subida de ficheros) siguen en pie y la fase 0 no se
toca.

### 7.6 Fase 6 — Clientes web y móvil. **Obligatorios**

**Decisión tomada: entran en el roadmap, no son «trabajo de otro equipo».**
Ningún cliente IRC existente sirve para esto, y un servidor sin cliente no es
un producto.

- **SDK de protocolo (TypeScript)** sobre el WebSocket que ya existe, hablando
  IRCv3 con las extensiones de las fases anteriores. Es la pieza común a web y
  móvil y la primera que se escribe.
- **Cliente web.** Canales, hilos, reacciones, historial con scroll infinito,
  adjuntos, llamadas, pantalla compartida.
- **Cliente móvil** (iOS y Android). Aquí aparecen dos cosas que el web no
  obliga a resolver y que condicionan al servidor:
  - **Notificaciones push** (APNs/FCM): un módulo con *workers* para el HTTP
    saliente, alimentado por las menciones y los mensajes directos. Sin esto un
    cliente móvil no sirve para nada.
  - **Reconexión y caché offline**: el cliente vuelve tras horas sin red y
    tiene que reconciliar. Es `CHATHISTORY` + `MARKREAD`, y es la razón de que
    la fase 2 tenga que estar antes que ésta.
- **El equipo de cliente puede empezar en la fase 0** contra un servidor de
  pruebas: el SDK y la UI no necesitan esperar al historial.

Con los clientes llega el resto del producto: búsqueda sobre el historial,
integraciones y bots (`bot.c` y `Service{}` ya son la base correcta), consola de
administración, auditoría (sobre §5.7), retención y exportación.

### 7.7 Fase 7 — Aislamiento de módulos

**Decisión tomada: hay que considerarlo.** El planteamiento es correcto: hoy un
módulo con un fallo de memoria corrompe el estado del core, y por el efecto de
un *split* la avería se nota en toda la red.

Hay que decir con claridad qué es posible: **aislar código nativo dentro del
propio proceso no lo es.** Un módulo cargado con `dlopen()` comparte el espacio
de direcciones y puede escribir en cualquier sitio; no hay barrera que poner.
Las opciones reales son tres:

| Opción | Qué protege | Qué cuesta |
|---|---|---|
| **seccomp-bpf + rlimits** | Syscalls y consumo de recursos. **No** protege la memoria del core | Bajo. Mitigación parcial, se puede hacer ya |
| **Módulo fuera de proceso** | Todo: memoria, caídas, bucles. Un módulo que muere no se lleva nada | Alto: toda la API de módulos pasa a ser IPC |
| **WebAssembly** (wasmtime, wasm3) | Memoria, en el mismo proceso | Alto: la API pasa a funciones anfitrionas, recompilar a wasm32, sin hilos, penalización de rendimiento |

**Propuesta: niveles de confianza declarados en la configuración.**

```
Module { name = "history";    isolation = "native";  };   # como hoy
Module { name = "integracion_x"; isolation = "process"; }; # fuera de proceso
```

- `native` — lo que hay hoy. Para módulos propios, revisados, en el camino
  crítico: historial, identidad, rtc.
- `process` — un proceso anfitrión por módulo, con la misma API por encima de
  un socket. Para lo que no controlamos: integraciones, módulos de terceros,
  cualquier cosa que ejecute lógica de un cliente.

**Lo que hace esto viable es la fase 0.** Un módulo fuera de proceso responde
por fuerza de forma asíncrona: sin `HOOK_PENDING` (§5.5) un hook de veto
remoto bloquearía el servidor en cada llamada, que es exactamente el problema
que se quería evitar. Es decir: el aislamiento no es una fase independiente que
se pueda adelantar — **depende de §5.5 y hay que construirlo después**.

Mientras tanto, y desde ya, lo barato: `RLIMIT_*` sobre el proceso, revisión
obligatoria con ASan y TSan para todo módulo del camino crítico (los presets ya
existen), y la regla de que ningún módulo de terceros entra en `native`.

---

## 8. Orden, dependencias y esfuerzo

```
  F0 Cimientos (core, ABI 8)
   │   capacidades dinámicas ✅ · hooks de comando ✅ · msgid ✅
   │   batch + labeled-response ✅ · multiline ✅ · cripto ircd_* ✅
   │   hooks async
   │
   ├──► F1 Identidad (SASL + ACCOUNT en core, email → hasta 3 nicks)
   │     │
   │     ├──► F2 Historial (PostgreSQL) ──► F3 Conversación ──┐
   │     │                                                     │
   │     └──► F5 HTTP + ficheros ───────────────────────────────┤
   │                                                            ├──► F6 Clientes
   ├──► F4 Texto enriquecido ───────────────────────────────────┘     web y móvil
   │
   └──► (F0 §5.5) ──► F7 Aislamiento de módulos

  Aplazado, sin dependencias en ningún sentido: voz, vídeo y pantalla
  compartida (§7.5).
```

- **F0 bloquea todo**, y su primer tramo ya está hecho: con 7 posiciones de
  capacidad libres no cabía nada, y ahora hay 119.
- **F4 sólo depende de F0.** Puede ir en paralelo a F1 y F2 con otra persona.
- **F7 depende de §5.5**, no se puede adelantar.
- **El equipo de cliente puede empezar en F0** contra un servidor de pruebas.

Órdenes de magnitud, como orientación y no como compromiso (equipo pequeño,
desarrollador con soltura en el código):

| Fase | Esfuerzo aprox. | Riesgo |
|---|---|---|
| F0 Cimientos | 3–4 meses-persona (§5.1 hecho) | Medio. Toca el core; los hooks de comando y la cripto suman |
| F1 Identidad | 2–3 | Medio. El modelo email → nicks es nuevo, no un port |
| F2 Historial | 3–4 | Medio-alto. Escala y retención |
| F3 Conversación | 2–3 | Bajo, una vez hay `msgid` |
| F4 Texto enriquecido | 1–2 | Medio. La degradación a texto plano |
| F5 HTTP + ficheros | 2–3 | Medio. Infraestructura nueva |
| F6 Clientes web y móvil | 8–14 | Medio-alto. Dos plataformas más el SDK |
| F7 Aislamiento | 4–6 | Alto. Duplica la superficie de la API de módulos |

Sin WebRTC, el camino hasta un producto usable es **una mensajería de equipo
completa**: cuentas, historial buscable, hilos, reacciones, texto enriquecido,
ficheros y clientes en web y móvil. Que es, de hecho, lo que casi todo el mundo
usa de Slack casi todo el tiempo.

### Camino corto recomendado (demostración)
**Terminar F0 → F1 → F2 → el cliente web de F6.** Un servidor donde un equipo
entra con su cuenta, habla en canales, y al reconectar encuentra lo que se dijo
mientras no estaba. Es lo mínimo que distingue esto de un IRC con buena pinta.

## 9. Riesgos y decisiones que quedan abiertas

1. **La compatibilidad con clientes IRC tradicionales es requisito** (ver
   cabecera). Ya no es un riesgo abierto sino una restricción de diseño: cuesta
   trabajo en cada extensión — hay que pensar la degradación antes que la
   función — y a cambio quita el único riesgo que podía obligar a rehacer lo
   ya hecho. La forma de mantenerla es la de §5.4: la extensión va detrás de
   una capacidad y no cambia un byte de lo que recibe quien no la pidió.
2. **Un despliegue por organización** mientras no exista el modelo de
   workspaces (§6.3). Condiciona el modelo de negocio.
3. **El nick es el nombre visible** (§6.2). `NICKLEN` caracteres, único en toda
   la red, y sin un nombre de visualización que el producto pueda dar por
   bueno. Conviene comprobar pronto que la UI se sostiene con eso.
4. **Escala de P10.** El estado de canal se replica entero en cada servidor.
   Hay que medirlo antes de F2, no después.
5. **Un core de un solo hilo.** Mientras la persistencia, el hashing y el HTTP
   saliente vayan a *workers*, el modelo aguanta. La regla de que un worker no
   toca estado del core no se negocia.
6. **Cumplimiento normativo.** Historial + adjuntos = datos personales, y con
   el email como credencial, datos identificativos. Retención, exportación,
   borrado y cifrado en reposo se diseñan en F1 y F2.
7. **El email no puede filtrarse nunca** (§6.2). Es una propiedad que hay que
   verificar con pruebas, no confiarla a la revisión.
8. **Retomar WebRTC** (§7.5) será una propuesta nueva, con la advertencia de
   esfuerzo que ya quedó escrita allí.

## 10. Lo que este documento recomienda no hacer

- **No** meter el proveedor HTTP ni el SFU dentro del proceso del ircd.
- **No** hacer pasar medios por el ircd, en ninguna forma.
- **No** llamar a un módulo desde otro: no se puede (`RTLD_LOCAL`), y el patrón
  correcto es el registro en el core (§4).
- **No** ejecutar Argon2 ni bcrypt en el hilo principal.
- **No** escribir en base de datos desde el hilo principal.
- **No** inventar un formato de texto enriquecido propietario.
- **No** empezar por las funciones vistosas (reacciones, hilos) antes de F0: se
  reescriben enteras en cuanto exista `msgid`.
- **No** ampliar `ircd_parser.y` módulo a módulo: el registro de bloques de
  configuración se hace en F0 y se paga una sola vez.
- **No** volver sobre MongoDB: la decisión está tomada y el razonamiento
  anotado en §7.1.
- **No** retomar WebRTC de forma oportunista dentro de otra fase: cuando se
  retome, será con su propia propuesta (§7.5).

---

## 11. Estado y siguiente paso

**Hecho** — la fase 0 está cerrada:

- **§5.1, capacidades dinámicas.** Mapa de bits de 128 posiciones, registro en
  `ircd/capab.c`, `module_add_cap()` en el API, ABI 8.
- **§5.7, hooks genéricos de comando.** Dos puntos en el despacho, el sujeto
  declarado por cada comando y resuelto una vez.
- **§5.4, identificadores de mensaje.** La línea es la unidad; cruza P10.
- **§5.3, `BATCH` y `labeled-response`.** Sin almacenar nada.
- **§5.2, `draft/multiline`.** Los trozos salen por el relay de siempre.
- **§5.8, criptografía `ircd_*`.** SHA-256, HMAC, AES-256-GCM, BLAKE2b,
  Argon2id y la forma almacenable de una contraseña, con vectores oficiales.
- **§5.5, hooks asíncronos.** `HOOK_PENDING` con plazo, congelación del nick
  mientras dura y denegación por defecto al expirar.

Todo lo de protocolo respeta la regla de compatibilidad: un cliente que no
negocia nada recibe la línea de siempre, byte a byte. Lo de criptografía no
cambia nada de lo que ve un cliente todavía: son primitivas, y su primer
consumidor es la identidad (§6).

Queda pendiente de §5.6 el resto de puntos de extensión —
`module_add_isupport()`, `module_add_feature()`, `module_add_config_block()`,
`module_add_numeric()`—, que no bloquean la fase 1 y se pagan cuando el primer
módulo los necesite.

**Siguiente paso: la identidad (§6).** Es lo que consume lo que la fase 0 dejó
puesto: `ircd_pwhash_*` en un *worker* para verificar sin parar el servidor,
`HOOK_PENDING` para retener el registro mientras esa verificación ocurre, y
`msgid`/`batch` para lo que venga después. Tendrá su propia propuesta, con el
modelo de correo → hasta tres cuentas-nickname y el regreso de SASL y del
comando `ACCOUNT` como piezas de core.

Cada fase será una propuesta con su propio documento. Las que ya se sabe que lo
necesitan: el modelo de identidad (§6), el aislamiento de módulos (§7.7) y,
cuando se retome, WebRTC (§7.5).
