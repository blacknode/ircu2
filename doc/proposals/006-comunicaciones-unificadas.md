# Propuesta 006 — De IRC a comunicaciones unificadas: texto enriquecido, voz, vídeo y pantalla compartida

**Estado:** hoja de ruta, aceptada parcialmente (revisión 2)
**Depende de:** 001 (API de módulos), 002 (hilos), 003 (modos por módulo),
004 (configuración desde el entorno), 005 (traducciones)
**Introduce:** nada todavía; define el orden en que se introduce lo demás

**Cambios respecto a la revisión 1** (decisiones tomadas):
1. Se añaden **hooks genéricos de comando** (`HOOK_COMMAND_PRE`/`POST`), §5.7.
2. El historial evalúa **MongoDB** además de PostgreSQL, §7.1.
3. Las cuentas se basan en **email**; un email agrupa varias cuentas, §6.2.
4. **Workspaces/namespaces: descartado por ahora**, §6.3.
5. **SASL y `ACCOUNT` vuelven, y van al core**, §6.1.
6. **SHA-256, AES-256, Argon2 y bcrypt al core** con prefijo `ircd_*`, §5.8.
7. **RTC con implementación propia**, sin adoptar un SFU de terceros, §7.5.
8. **Cliente web y móvil entran en el roadmap** como fase propia, §7.7.
9. **HTTP es un módulo**, con registro en el core y comprobación por el
   consumidor, §7.4.
10. **Aislamiento de módulos** pasa a ser una fase, §7.8.

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

### 3.4 `capset_t` es de 16 bits — el bloqueo duro
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

### 3.6 Los hooks son síncronos
`HOOK_PENDING` está reservado y sin implementar (`hooks.h`). Autenticar contra
una base de datos, o verificar un token SSO, hoy bloquearía el servidor entero.

### 3.7 No hay visibilidad sobre los comandos
No existe ningún punto donde un módulo vea «este usuario va a hacer X sobre
este otro usuario». Auditoría, antiabuso, permisos por rol y registro de
acciones son hoy imposibles sin parchear cada `m_*.c`. Ver §5.7.

### 3.8 No hay HTTP
El WebSocket es sólo un *upgrade* que transporta líneas IRC.

### 3.9 Los módulos no tienen aislamiento
Un módulo con un fallo de memoria corrompe el estado del core y tumba el
servidor — y, por el efecto de un split, perturba la red. Ver §7.8.

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

### 5.1 Capacidades dinámicas
- `capset_t` de 16 bits → mapa de bits de tamaño fijo (como `flag_t` en
  `user_flags.h`), con 64–128 posiciones.
- Tabla de capacidades en tiempo de ejecución, como se hizo con los modos en
  003: `module_add_cap(mod, "draft/react", flags, &cap)` devuelve la posición,
  y se revierte al descargar el módulo con `CAP DEL` a quien tenga
  `cap-notify`. `cap_new()`/`cap_del()`/`cap_update_availability()` ya existen.

### 5.2 Longitud de línea
- `draft/multiline` + `BATCH`: la respuesta estándar y federable a los 512
  bytes, sin romper a ningún cliente antiguo.
- Subir el buffer de reescritura de hooks por encima de `BUFSIZE`.
- `TOPICLEN`/`AWAYLEN` como *features*, no como constantes.

### 5.3 `BATCH` y `labeled-response`
`BATCH` es prerrequisito de multiline, de chathistory y de cualquier entrega
agrupada. `labeled-response` es lo que permite a un cliente correlacionar
petición y respuesta — imprescindible para una UI que no sea un terminal.

### 5.4 Identificadores de mensaje (`msgid`)
Tag `msgid` estable y único por red, generado en el servidor de origen y
federado en P10. **Es la clave primaria de todo lo que viene después**:
historial, hilos, reacciones, ediciones, borrados y marcas de leído. Sin
`msgid` ninguna de esas cinco cosas se puede construir.

### 5.5 Hooks asíncronos (`HOOK_PENDING`)
Implementar el valor ya reservado: un hook devuelve `HOOK_PENDING`, la
operación queda suspendida con su contexto, y el módulo la reanuda con
`hook_resume(token, HOOK_ALLOW|HOOK_DENY)` cuando su consulta termina.
Requiere congelar el estado del cliente mientras tanto y un plazo máximo.
Es lo que hace posible autenticar contra un almacén externo sin parar el
servidor — y, más adelante, lo que hace viable el aislamiento de módulos
(§7.8), porque un módulo fuera de proceso responde por fuerza de forma
asíncrona.

### 5.6 Puntos de extensión que faltan
- `module_add_isupport()` — tokens en el 005 desde un módulo.
- `module_add_feature()` — *features* propias, visibles en `/GET` y `/SET`.
- `module_add_config_block()` — bloques de configuración propios, en vez de
  ampliar `ircd_parser.y` por cada módulo.
- `module_add_numeric()` — numerics propios, con su contexto de traducción.
- Hooks nuevos: `HOOK_MESSAGE_DELIVERED` (incluido el origen remoto, §3.5),
  `HOOK_CHANNEL_TOPIC_CHANGED`, `HOOK_CLIENT_AWAY`, `HOOK_PRESENCE_CHANGED`.

### 5.7 Hooks genéricos de comando — propuesta

**Problema.** Hoy no hay forma de que un módulo observe o vete una acción de un
usuario sobre otro: `KICK`, `KILL`, `WHOIS`, `GLINE`, `SLINE`, `JUPE`, `MODE`,
`INVITE`, `SILENCE`. Añadir un hook por comando no escala: son decenas, y cada
comando nuevo — del core o de un módulo — necesitaría el suyo.

**Diseño.** Dos puntos genéricos en el despacho, no en cada `m_*.c`:

```c
enum HookType {
  ...
  HOOK_COMMAND_PRE,    /* antes del handler; puede vetar */
  HOOK_COMMAND_POST,   /* después del handler; notificación */
};
```

`ircd/parse.c` tiene exactamente **dos** puntos de despacho, y los dos son un
`return (*handler)(cptr, from, i, para)` — uno en `parse_client()` (línea 1118)
y otro en `parse_server()` (línea 1433). Los hooks se instrumentan ahí y en
ningún otro sitio.

**Qué recibe el módulo.** `struct HookContext` gana un puntero a un descriptor
del comando:

```c
/** El comando que se está despachando. */
struct HookCommand {
  const char*       hcc_cmd;      /**< "KICK". */
  const char*       hcc_tok;      /**< "K". */
  enum HandlerType  hcc_handler;  /**< Qué handler corre (CLIENT, OPER, ...). */
  int               hcc_parc;     /**< Número de parámetros. */
  char* const*      hcc_parv;     /**< Los parámetros, de sólo lectura. */
  int               hcc_result;   /**< Sólo en POST: lo que devolvió el handler. */
};
```

y el contexto que ya existe se rellena con lo que el comando declare:

- `hc_source`  — quién ejecuta (el usuario de origen, local o remoto).
- `hc_client`  — sobre quién se ejecuta, ya resuelto a `struct Client*`.
- `hc_channel` — el canal implicado, si lo hay.
- `hc_arg`     — el texto libre (la razón de un `KICK`, de un `KILL`, de un
                 `GLINE`), si el comando lo declara.
- `hc_command` — el `struct HookCommand` de arriba.

**Cómo se resuelve el objetivo.** El despachador no puede saber genéricamente
qué parámetro nombra a la víctima: eso lo declara cada comando. `struct
Message` gana un descriptor pequeño:

```c
/** Qué parámetro de este comando nombra a qué.  -1 = no aplica. */
struct MsgSubject {
  signed char ms_target;    /**< parv[] con un nick o numnick. */
  signed char ms_channel;   /**< parv[] con un canal. */
  signed char ms_mask;      /**< parv[] con una máscara user@host. */
  signed char ms_reason;    /**< parv[] con el texto libre. */
};
```

y `msgtab[]` se rellena para los comandos que son una acción de alguien sobre
alguien:

| Comando | target | channel | mask | reason |
|---|---|---|---|---|
| `KICK`    | 2  | 1  | -1 | 3  |
| `KILL`    | 1  | -1 | -1 | 2  |
| `WHOIS`   | 1/2| -1 | -1 | -1 |
| `INVITE`  | 1  | 2  | -1 | -1 |
| `MODE`    | 1  | 1  | -1 | -1 |
| `SILENCE` | 1  | -1 | -1 | -1 |
| `GLINE`   | -1 | -1 | 1  | 3  |
| `SLINE`   | -1 | -1 | 1  | 3  |
| `JUPE`    | -1 | -1 | 1  | 4  |

`module_add_command()` gana una variante que acepta el mismo descriptor, de
forma que un comando de módulo participa en los hooks igual que uno del core.
El descriptor sirve además, gratis, para auditoría y para registro estructurado.

**Coste.** Un hook que se dispare en cada línea de un enlace P10 saturado es
inaceptable. Por eso el registro es **por comando**:

```c
extern int hook_add_command(struct ModuleHandle* mod, enum HookType type,
                            const char* cmd,   /* NULL = todos */
                            HookFn fn, int priority, void* user,
                            unsigned int flags);
```

Cada `struct Message` lleva un contador de escuchantes que se incrementa al
registrar. El despachador comprueba ese contador: es **O(1) y cuesta cero
cuando nadie escucha ese comando**, que es el caso normal.

**Reglas, y por qué cada una:**

1. **Origen `+S` excluido.** Ni `PRE` ni `POST` se disparan cuando
   `IsServiceBot(sptr)`. Un servicio actúa en nombre del servidor y no debe ser
   auditado ni vetado por un módulo. Un módulo de auditoría que sí los necesite
   lo pide explícitamente con `HOOK_CMD_INCLUDE_SERVICES` en `flags`.
2. **El veto sólo vale en local.** `HOOK_DENY` en `PRE` se honra únicamente
   cuando el origen es un cliente de este servidor (`MyConnect(sptr)`). Para un
   comando que llega de otro servidor el hook es **sólo notificación**, y un
   `DENY` se registra en el log y se ignora: vetar en un solo servidor un
   cambio de estado que el resto de la red ya aplicó es desincronizarla.
3. **`POST` no corre si el handler devolvió `CPTR_KILLED`.** El cliente ya no
   existe y los punteros del contexto son memoria liberada. El módulo se entera
   de la salida por `HOOK_CLIENT_EXITING`, que es donde corresponde.
4. **`parv` es de sólo lectura en la versión 1.** Reescribir parámetros
   arbitrarios de un comando en vuelo es una fuente de fallos difícil de
   acotar. La reescritura del texto libre (`ms_reason`), que sí es tratable,
   queda para una versión posterior con el mecanismo de `hc_rewrite` que ya
   existe.
5. **Guardia de reentrada.** Un hook que provoque el despacho de otro comando
   incrementa una profundidad; pasado un límite pequeño se aborta y se registra.
6. **`HOOK_PENDING` en `PRE`** queda habilitado en cuanto exista §5.5: suspende
   el comando hasta que el módulo responda. Es lo que permite un permiso
   consultado en base de datos sin bloquear.

**Ejemplo.**

```c
static enum HookResult
audit_pre(struct HookContext* ctx, void* user)
{
  if (0 == ircd_strcmp(ctx->hc_command->hcc_cmd, "KICK")
      && es_protegido(ctx->hc_client)) {
    ctx->hc_numeric = ERR_CHANOPRIVSNEEDED;
    ircd_strncpy(ctx->hc_reason, "ese usuario está protegido",
                 sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }
  return HOOK_CONTINUE;
}

static int
mi_init(struct ModuleHandle* mod)
{
  hook_add_command(mod, HOOK_COMMAND_PRE, "KICK", audit_pre,
                   HOOK_PRIORITY_DEFAULT, NULL, 0);
  hook_add_command(mod, HOOK_COMMAND_POST, NULL, audit_post,
                   HOOK_PRIORITY_DEFAULT, NULL, 0);
  return 0;
}
```

**Lo que esto habilita sin más trabajo de core:** auditoría completa,
antiabuso, permisos por rol (necesario para un producto con administradores),
protección de usuarios, límites de tasa por comando, y registro estructurado de
toda acción de un usuario sobre otro — que en un producto de empresa es
requisito, no mejora.

### 5.8 Criptografía en el core: `ircd_sha256`, `ircd_aes`, `ircd_argon2`, `ircd_bcrypt`

Cuatro ficheros nuevos, con el prefijo del proyecto:

| Fichero | Para qué |
|---|---|
| `ircd/ircd_sha256.c` | HMAC-SHA256 de tokens (SFU, subidas), huellas de certificado, SCRAM |
| `ircd/ircd_aes.c` | AES-256-**GCM** para sellar tokens y cifrar secretos en reposo |
| `ircd/ircd_argon2.c` | Hash de contraseñas, algoritmo recomendado |
| `ircd/ircd_bcrypt.c` | Hash de contraseñas, para compatibilidad con lo existente |

Cuatro reglas:

1. **No pueden depender del backend TLS.** `IRCU_TLS` puede valer `none`, y el
   backend puede ser GnuTLS o libtls. Van implementaciones propias empotradas,
   con aceleración opcional cuando el backend es OpenSSL. Todas las referencias
   habituales son de licencia compatible con GPL (libargon2 CC0/Apache-2.0,
   `crypt_blowfish` de Openwall en dominio público).
2. **Argon2 y bcrypt no corren en el hilo principal.** Están diseñados para ser
   lentos: un hash decente cuesta entre 50 y 250 ms. A 100 ms por verificación,
   diez autenticaciones por segundo consumen el servidor entero. **Van por
   `worker_submit()` sin excepción**, y por eso la autenticación necesita los
   hooks asíncronos de §5.5. Esta dependencia no es negociable y ordena las
   fases: §5.5 antes que §6.
3. **AES-256 se usa en modo autenticado (GCM), nunca en crudo.** Un token
   cifrado sin autenticar es un token manipulable.
4. **Entran por el registro que ya existe.** `ircd/ircd_crypt.c` ya tiene un
   registro de mecanismos (`native`, `plain`, `smd5`): Argon2 y bcrypt se
   añaden ahí, no por un camino paralelo.

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

### 6.2 El modelo: un email, varias cuentas

**Decisión tomada.** La credencial es el **email**; una cuenta es una identidad
en la red; **un email agrupa varias cuentas**.

```
  identity (email)                    account
  ────────────────                    ───────
  email          ◄── credencial       name        ◄── lo que se ve en la red
  password_hash      (argon2)         identity_id
  mfa_secret         (aes-256-gcm)    display_name
  sso_subject                         created_at
  verified_at                         is_default
  created_at                          suspended_at
       │                                   ▲
       └───────────── 1 : N ───────────────┘
```

Consecuencias, todas deliberadas:

- **Una persona, varias identidades en la red.** Cuenta personal y cuenta de
  bot bajo el mismo email; una persona con dos roles; alias separados. Es lo
  que Undernet nunca permitió y lo que un producto moderno da por supuesto.
- **La recuperación, la verificación, el 2FA y el SSO cuelgan del email**, no
  de la cuenta. Se implementan una vez.
- **Al autenticar hay que elegir cuenta.** `ACCOUNT LOGIN <email> <password>
  [<cuenta>]`; sin el tercer parámetro se usa la marcada por defecto. En SASL
  el mismo dato viaja en el `authzid` (la identidad que se quiere asumir),
  que es exactamente para lo que existe ese campo en el protocolo.
- **El email no se publica nunca.** No aparece en `WHOIS`, ni en ningún
  numeric, ni cruza P10. Lo que viaja por la red es el nombre de cuenta. Esto
  es requisito de privacidad y hay que imponerlo en el código, no confiarlo al
  criterio de quien escriba el siguiente módulo.
- **El nick sigue siendo el identificador de enrutado**, distinto de la cuenta.
  El nombre visible va por `setname`/`metadata-2`.

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

## 7. Fases 2 a 8 — El producto

### 7.1 Fase 2 — Historial, y la evaluación de MongoDB

#### La corrección técnica primero

**La búsqueda en MongoDB no es O(1).** Conviene fijarlo antes de decidir sobre
esa base:

- Los índices de MongoDB son **B-tree**: una búsqueda por igualdad es
  **O(log n)**, igual que en PostgreSQL.
- MongoDB tiene índices `hashed`, que sí son de dispersión, pero **sólo sirven
  para igualdad exacta**: no sirven para rangos ni para ordenar. Su propósito
  real es repartir *shards*, no acelerar consultas.
- El acceso del historial **es intrínsecamente un rango**: `CHATHISTORY BEFORE
  <msgid|timestamp> LIMIT 50` es «los 50 anteriores a este punto, en orden».
  Eso necesita un índice **ordenado**, de forma que un índice de dispersión no
  es aplicable ni en Mongo ni en ningún otro motor.
- La **búsqueda de texto** no es O(1) en ningún motor: es un índice invertido.
  Mongo ofrece `$text` (limitado) o Atlas Search (Lucene, servicio aparte);
  PostgreSQL ofrece `tsvector` + GIN. Comparables.

Con la consulta real del producto — un rango ordenado por tiempo dentro de un
canal — **los dos motores dan lo mismo: O(log n) para localizar el extremo del
rango y lectura secuencial del resto**. La elección, por tanto, no se decide
por complejidad algorítmica.

#### Dónde sí se diferencian

| | PostgreSQL (ya integrado) | MongoDB |
|---|---|---|
| Escalado de escritura | Vertical; particionado declarativo por tiempo | **Sharding horizontal nativo** |
| Índices para series temporales | **BRIN**: minúsculo y perfecto para datos ordenados en el tiempo | Índices normales |
| Retención automática | Borrado de particiones (instantáneo) | **Índices TTL** |
| Esquema flexible por mensaje | JSONB | Nativo |
| Búsqueda de texto | GIN + `tsvector`, integrado | `$text` (flojo) o Atlas Search (aparte) |
| Transacción con las cuentas | **Sí, es la misma base** | No (bases distintas) |
| Notificación de cambios | `LISTEN`/`NOTIFY` | **Change streams** |
| Coste de integración aquí | **Cero: ya está hecho** | Alto, ver abajo |

#### El coste real de un driver MongoDB

No es escribir el driver: es que **`include/db.h` está diseñado sobre SQL**.
`struct DbQuery` es texto SQL con marcadores `$n`; `struct DbParam` son
parámetros posicionales; `include/migration.h` son *scripts* SQL con `up`/`down`
y cada migración es una transacción. Nada de eso describe una operación de
Mongo. Un driver de Mongo obliga a:

1. generalizar `struct DbQuery` a una segunda forma (documento/comando), con el
   riesgo de convertir la abstracción en el mínimo común denominador de dos
   motores que no se parecen;
2. decidir qué significa una migración cuando no hay esquema;
3. añadir `libmongoc` como dependencia de compilación del módulo.

#### Recomendación

**Empezar en PostgreSQL. No descartar Mongo, aplazarlo.** En concreto:

- El módulo `history` define **su propia interfaz interna de almacén** y la
  implementa primero sobre el driver PostgreSQL. La forma de la consulta —
  `latest`, `before`, `after`, `around`, `between`, `targets` — se expresa en
  esa interfaz, no en SQL suelto por el código.
- Tabla particionada por mes, índice BRIN sobre el tiempo, GIN para búsqueda,
  clave primaria `msgid`.
- **El criterio para traer Mongo es medido, no estético:** cuando el volumen de
  escritura sostenido exceda lo que una instancia de PostgreSQL con
  particionado absorbe, y el cuello sea de escritura y no de consulta. En ese
  punto el trabajo es una implementación más de la interfaz interna, y la
  generalización de `db.h` se hace con un caso de uso real delante en vez de
  por anticipado.
- Si se decide adoptar Mongo igualmente desde el principio, **el prerrequisito
  es la generalización de `struct DbQuery`**, y eso pertenece a la fase 0.

#### El resto de la fase

- Captura en `HOOK_MESSAGE_DELIVERED` (§5.6), no en el hook de origen local.
- Escrituras por el driver asíncrono; **nunca** en el hilo del event loop.
- `CHATHISTORY LATEST|BEFORE|AFTER|AROUND|BETWEEN|TARGETS`, dentro de un `BATCH`.
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

### 7.5 Fase 6 — Voz, vídeo y pantalla compartida, con implementación propia

**Decisión tomada: no se adopta un SFU de terceros.** Lo que sigue respeta esa
decisión y la hace ejecutable, con una advertencia dicha una sola vez y en
serio: **un SFU completo es un producto por derecho propio** — ICE, DTLS-SRTP,
RTP/RTCP, simulcast, estimación de ancho de banda, NACK/PLI, jitter, recuperación
de pérdidas. Escrito desde cero, de verdad desde cero, son años. La manera de
hacerlo propio sin que eso ocurra es la de esta fase: **implementación propia
sobre primitivas, no sobre un SFU ajeno**, y en tres saltos que entregan valor
cada uno.

```
  Cliente ──── señalización (WebSocket/IRC) ────►  ircd  ── módulo rtc
     │                                               │     (quién puede, a qué sala)
     │                                               └──►  emite credenciales TURN
     │                                                     y tokens de sala
     │
     └──────────── medios (SRTP/DTLS) ──────────►  F6a: el otro cliente (P2P)
                                                   F6b: nuestro SFU
```

#### F6a — Señalización y llamadas P2P. **Aquí ya hay producto.**

La observación que ordena toda la fase: **una llamada de 1 a 1, y un grupo
pequeño en malla, no necesitan SFU en absoluto.** WebRTC conecta a los pares
directamente. Con sólo el módulo de señalización y un TURN, se tienen llamadas
de voz, de vídeo **y pantalla compartida** funcionando entre dos personas y en
grupos de hasta tres o cuatro.

- Módulo `rtc` con comando y token P10 propios:
  `RTC START|JOIN|LEAVE|OFFER|ANSWER|CANDIDATE|END`.
- **Autorización:** quién puede iniciar una llamada en un canal y quién puede
  unirse sale de la pertenencia y de los modos — el estado que el ircd ya tiene
  y que nadie más tiene. Ésta es la razón de que la señalización viva aquí.
- **Estado visible:** modo de canal registrado por el módulo, lista de
  participantes, presencia en la UI.
- **TURN:** credenciales REST efímeras (HMAC-SHA1, ya disponible; o SHA-256 de
  §5.8). Se usa **coturn**: un TURN es un relé UDP autenticado, está resuelto
  desde hace quince años, y escribir uno propio no aporta nada al producto.
- **Pantalla compartida no es una función aparte:** es una pista de vídeo más
  (`getDisplayMedia`) con su etiqueta. Si la llamada funciona, compartir
  pantalla es trabajo de cliente.

#### F6b — SFU propio

El salto necesario cuando una sala pasa de cuatro personas: en malla, cada
participante envía su vídeo a todos los demás, y el ancho de banda de subida
crece con el cuadrado del grupo.

**Implementación propia significa que el router es nuestro, no que
reimplementemos los protocolos de transporte.** Sobre estas primitivas:

| Pieza | Con qué | Por qué no propio |
|---|---|---|
| ICE / STUN | `libjuice` o `libnice` | Protocolo cerrado, sin valor diferencial |
| DTLS | OpenSSL | Igual |
| SRTP | `libsrtp2` | Igual; además es la referencia del IETF |
| SCTP (data channels) | `usrsctp` | Igual |
| Todo lo anterior junto | `libdatachannel` (C++, API en C) | Atajo razonable para F6b |

**Lo que sí escribimos nosotros, y es donde está el producto:** el router de
paquetes, la selección de capa en simulcast, la estimación de ancho de banda
(TWCC), la gestión de keyframes (PLI/FIR), la pertenencia a salas y su enlace
con la autorización del ircd, y las métricas.

El SFU es un **proceso aparte**, no un módulo del ircd: un fallo en el camino
de medios no puede tumbar la red. Habla con el ircd por la interfaz que el
módulo `rtc` defina, y valida los tokens de sala que el ircd emite.

#### F6c — Grabación, transcripción, salas grandes

Sale del SFU hacia afuera y no toca el ircd.

### 7.6 Ordenación realista de la fase 6

F6a depende sólo de la fase 0 y de la autenticación. **Es el camino más corto a
una demostración con efecto** y conviene hacerlo pronto, incluso antes que el
historial. F6b es un proyecto en sí mismo y merece su propia propuesta.

### 7.7 Fase 7 — Clientes web y móvil. **Obligatorios**

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

### 7.8 Fase 8 — Aislamiento de módulos

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
   │   caps dinámicas · batch/labeled · msgid · multiline
   │   hooks async · hooks de comando · cripto ircd_*
   │
   ├──► F1 Identidad (SASL + ACCOUNT en core, cuentas por email)
   │     │
   │     ├──► F2 Historial ──► F3 Conversación ──┐
   │     │                                        │
   │     ├──► F5 HTTP + ficheros ─────────────────┤
   │     │                                        ├──► F7 Clientes web y móvil
   │     └──► F6a Señalización + P2P ─────────────┤
   │                │                             │
   │                └──► F6b SFU propio ──────────┘
   │
   ├──► F4 Texto enriquecido
   │
   └──► (F0 §5.5) ──► F8 Aislamiento de módulos
```

- **F0 bloquea todo.** Con 7 bits de capacidad libres no caben las extensiones.
- **F6a puede ir justo después de F0 y de la autenticación de F1.** Es el
  camino más corto a una demostración con voz, vídeo y pantalla compartida.
- **F4 sólo depende de F0.**
- **F8 depende de §5.5**, no se puede adelantar.

Órdenes de magnitud, como orientación y no como compromiso (equipo pequeño,
desarrollador con soltura en el código):

| Fase | Esfuerzo aprox. | Riesgo |
|---|---|---|
| F0 Cimientos | 3–4 meses-persona | Medio. Toca el core; los hooks de comando y la cripto suman |
| F1 Identidad | 2–3 | Medio. El modelo email→cuentas es nuevo, no un port |
| F2 Historial | 3–4 | Medio-alto. Escala y retención |
| F3 Conversación | 2–3 | Bajo, una vez hay `msgid` |
| F4 Texto enriquecido | 1–2 | Medio. La degradación a texto plano |
| F5 HTTP + ficheros | 2–3 | Medio. Infraestructura nueva |
| F6a Señalización + P2P | 2–3 | Medio. WebRTC y NAT |
| F6b SFU propio | **12–24** | **Alto. Es un producto aparte** |
| F7 Clientes web y móvil | 8–14 | Medio-alto. Dos plataformas más el SDK |
| F8 Aislamiento | 4–6 | Alto. Duplica la superficie de la API de módulos |

### Camino corto recomendado (demostración)
**F0 → autenticación de F1 → F6a.** Un servidor en el que un equipo entra con
su cuenta, habla en canales y hace llamadas con vídeo y pantalla compartida,
sin SFU y sin historial. Es la demostración que justifica el resto.

---

## 9. Riesgos y decisiones que quedan abiertas

1. **F6b es el riesgo principal del proyecto.** Un SFU propio puede consumir
   más esfuerzo que todo el resto del roadmap junto. La mitigación es F6a: se
   entrega producto antes de comprometerse con F6b, y la decisión se toma con
   usuarios reales delante.
2. **Un despliegue por organización** mientras no exista el modelo de
   workspaces (§6.3). Condiciona el modelo de negocio.
3. **¿Se mantiene la compatibilidad con clientes IRC estándar?** Si es
   requisito, toda extensión debe degradar limpiamente y nada puede ser
   obligatorio. Si no lo es, se puede ir más rápido y con un protocolo más
   limpio. **Afecta a todas las fases y sigue sin decidirse.**
4. **Escala de P10.** El estado de canal se replica entero en cada servidor.
   Hay que medirlo antes de F2, no después.
5. **Un core de un solo hilo.** Mientras la persistencia, el hashing y el HTTP
   saliente vayan a *workers*, el modelo aguanta. La regla de que un worker no
   toca estado del core no se negocia.
6. **Cumplimiento normativo.** Historial + adjuntos + grabación = datos
   personales, y con el email como credencial, datos identificativos.
   Retención, exportación, borrado y cifrado en reposo se diseñan en F1 y F2.
7. **El email no puede filtrarse nunca** (§6.2). Es una propiedad que hay que
   verificar con pruebas, no confiarla a la revisión.

---

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
- **No** adoptar MongoDB antes de tener una medida que lo justifique (§7.1).
- **No** escribir un TURN propio: coturn resuelve eso y no es diferencial.

---

## 11. Primer paso concreto

El primer cambio de código es pequeño, aislado y verificable:

1. `capset_t` de 16 bits → mapa de bits, con la lista de capacidades en tiempo
   de ejecución (el patrón que la propuesta 003 usó para los modos de canal).
2. `module_add_cap()` en el API, ABI 8.
3. Prueba unitaria del registro de capacidades y prueba de integración que
   cargue un módulo con una capacidad propia, verifique `CAP LS`, `CAP NEW` y
   `CAP DEL`, y compruebe que la descarga la retira de todos los clientes
   conectados.

En paralelo, y sin dependencias con lo anterior, los hooks de comando (§5.7):
son dos puntos en `ircd/parse.c`, un descriptor en `struct Message` y el
registro por comando, con `KICK`, `KILL` y `GLINE` como casos de prueba.

A partir de ahí, cada fase es una propuesta con su propio documento. Las que ya
se sabe que lo necesitan: los hooks de comando (§5.7), el modelo de identidad
(§6), el SFU propio (§7.5) y el aislamiento de módulos (§7.8).
