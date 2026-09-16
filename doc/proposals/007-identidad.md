# Propuesta 007 — Identidad: SASL, `ACCOUNT` y el modelo de cuentas

**Estado:** propuesta, revisión 2 (aprobada con correcciones).
**Es la fase 1 de la 006** (§6), que dejó tomadas las decisiones de fondo; esto
las convierte en un diseño con el que se puede escribir código.
**Depende de:** 001 (API de módulos), 006 §5.1 (capacidades dinámicas),
§5.5 (hooks asíncronos), §5.8 (criptografía `ircd_*`), y de `db.h` +
`migration.h` para los módulos.
**Introduce:** `include/sasl.h` + `ircd/sasl.c`, `include/account.h` +
`ircd/account.c`, `include/cache.h` + `ircd/cache.c`, `ircd/m_authenticate.c`,
`ircd/m_account.c`, el modo `+f`, `User::email`, opciones libres en el
bloque `Service{}`, `modules/services/identity/`,
`modules/services/nickserv/` y `modules/workers/redis/`.
**Revierte:** la retirada de SASL y de las cuentas documentada en
`doc/readme.accounting`, que se reescribe entera.

---

## 0. Correcciones de la revisión 1

Se aprobó con cambios, y tres de ellos invierten decisiones que la revisión 1
daba por cerradas. Van aquí en voz alta porque quien lea el código después va a
encontrarse con lo contrario de lo que decía la primera versión:

1. **El email sí se guarda en `struct User`**, en un miembro nuevo, y se
   muestra en `WHOIS` **al propio usuario y a nadie más**. La revisión 1 decía
   que no entraba en `struct Client` en absoluto. Sigue sin cruzar P10 y sigue
   sin verlo un tercero (§8).
2. **Fallar la autenticación no deja al cliente entrar sin identificar**: se le
   renombra a `guest-*`, y si ese nombre también está tomado, se le mata (§5).
3. **Si el servicio no puede comprobar a quién pertenece un nick, el nick no se
   puede usar** (§7). La revisión 1 decía que degradaba dejando entrar sin
   `+r`; eso es una invitación a suplantar a alguien esperando a que la base de
   datos se caiga.

Lo demás que cambia y no invierte nada: `identity` pasa a ser el mecanismo que
usa `nickserv` (§1), aparece el modo `+f` y la congelación (§6), `ACCOUNT` gana
`LIST` (§4.2), `LOGOUT` renombra (§5), el límite de cuentas por email es
configuración del bloque de `nickserv` (§9), y entra Redis delante de
PostgreSQL (§10).

---

## 1. Tres capas, no dos

La revisión 1 repartía entre el core y un módulo. Con `nickserv` en el dibujo
son tres, y la línea entre las dos últimas es la que hay que tener clara:

```
  core            el protocolo y struct Client
                  AUTHENTICATE, ACCOUNT, +r, +f, el email en User
                      │  (registro de proveedor, account.h)
                      ▼
  identity        el mecanismo: ¿esta credencial es correcta?
                  la base de datos, Redis, Argon2 en un worker
                  no habla con nadie, no tiene bot, no tiene comandos
                      │  (los mismos registros del core)
                      ▼
  nickserv        la política y la voz
                  el bot, los avisos, el plazo de gracia, /msg NickServ
                  sus opciones en el Service{} que ya lo declara
```

**`identity` es el mecanismo que `nickserv` usa.** Es el motivo por el que
`identity` no imprime un solo mensaje: si lo hiciera, `nickserv` no podría
cambiar el texto, ni traducirlo, ni decidir cuándo callarse, sin parchear el
módulo de debajo. `identity` responde preguntas —¿existe esta cuenta?, ¿es
correcta esta contraseña?, ¿de quién es este nick?— y `nickserv` decide qué
hacer con la respuesta y cómo contarlo.

**Quien habla con el usuario es `nickserv`, siempre.** El core manda numerics
(que son protocolo) y nada más. Los avisos —«ese nick está registrado»,
«te quedan treinta segundos», «te he renombrado»— salen del bot, por
`NOTICE`, como cualquier servicio de IRC.

---

## 2. Lo que no cruza P10, y por qué eso simplifica todo

**`AUTHENTICATE` no cruza el enlace. `SASL` no es un token P10. No hay servidor
de servicios al que enrutar la autenticación.**

En una red IRC clásica no es así: SASL se relaya a un servicio central porque
sólo él tiene las credenciales. Aquí `identity` corre en cada servidor y
consulta la **misma** base de datos y el **mismo** Redis, así que cada servidor
puede comprobar por sí mismo. Se eliminan el enrutado de SASL entre servidores,
su tabla de sesiones a medias, y un token P10 por cada vuelta del diálogo.

**Lo que viaja son dos modos que ya saben viajar.** `+r` sobre un cliente que
se llama `maria` dice, sin parámetros, «este `maria` es el de verdad» — porque
una cuenta *es* un nick. `+f` viaja igual, como cualquier modo global, para que
cada servidor sepa que ese cliente está congelado. Ninguno de los dos necesita
protocolo nuevo.

**El email no viaja.** Ni en el burst, ni en un `MODE`, ni en ningún sitio: es
un dato local de la conexión, puesto por el servidor que autenticó y por nadie
más (§8).

Consecuencia: `identity`, el driver de base de datos y el de Redis deben estar
cargados en **todos** los servidores. Uno que no los tenga no puede autenticar
a nadie ni proteger un nick, pero respeta el `+r` y el `+f` que le llegan.

---

## 3. Los registros del core

Con la forma que el árbol ya usa cuatro veces —modos de usuario y de canal,
capacidades, driver de base de datos—, y por el mismo motivo: un módulo no
resuelve los símbolos de otro (`RTLD_LOCAL`), así que el punto de encuentro lo
tiene el core.

### 3.1 Mecanismos SASL — `include/sasl.h`, `ircd/sasl.c` *(hecho)*

Lista en tiempo de ejecución, ordenada por nombre. El core trae `PLAIN`
(con `SASL_MECH_NEEDS_TLS`) y `EXTERNAL`; un módulo añade los suyos con
`module_add_sasl_mechanism()`. `sasl.c` convierte las líneas `AUTHENTICATE` en
una credencial —authcid, authzid, secreto— y ahí se para; no conoce clientes ni
sockets, y por eso se prueba entero sin servidor (`sasl_t`).

El valor de la capacidad (`sasl=PLAIN,EXTERNAL`) sale de recorrer el registro,
y la capacidad **sólo se ofrece si hay proveedor** (§3.2):
`cap_update_availability()` la retira cuando no lo hay. Un cliente que negocia
SASL contra un servidor que no puede autenticar a nadie está peor que uno que
ve que no se ofrece.

### 3.2 Proveedor de identidad — `include/account.h`, `ircd/account.c`

Un proveedor a la vez, registrado por `identity`, con la forma de
`db_register_driver()`:

```c
struct AccountProvider {
  const char* ap_name;
  /* Comprobar una credencial.  Copia lo que necesite de *req -- que es del
   * core y no sobrevive a la llamada -- y contesta más tarde con
   * account_complete(id, ...), en el hilo principal. */
  void (*ap_verify)(account_id_t id, const struct AccountRequest* req);
  /* ¿De quién es este nick?  Misma forma, misma respuesta diferida. */
  void (*ap_lookup)(account_id_t id, const char* nick);
  /* ¿Qué cuentas tiene esta dirección?  La que responde a ACCOUNT LIST. */
  void (*ap_list)(account_id_t id, const char* email);
  void (*ap_cancel)(account_id_t id);
};
```

Las tres son obligatorias. Un proveedor que verificara pero no supiera
listar haría que `ACCOUNT LIST` contestase «el servicio de identidad no está
disponible» en un servidor donde la identidad funciona, que es la única
respuesta que un usuario no puede distinguir de una caída.

`struct AccountRequest` lleva el mecanismo, el authcid, el authzid, el secreto,
la huella TLS y el numnick del cliente. **El secreto se borra con
`ircd_crypto_wipe()` en cuanto el proveedor lo ha copiado**, y el proveedor
está obligado a lo mismo.

`account_complete(id, resultado, nick, email, motivo)` devuelve el nick y el
email — el email porque la corrección 1 lo pide en `struct User`, y sólo ahí
(§8). Descargar el módulo falla toda petición en vuelo con
`ACCOUNT_ERR_UNAVAILABLE` antes de volver, como `db_unregister_driver()`.

### 3.3 Caché — `include/cache.h`, `ircd/cache.c` *(hecho)*

Tercer registro, misma forma, un driver: `modules/workers/redis/`. El core no
sabe qué es Redis; sabe que hay un almacén clave-valor con plazo de caducidad
al que se le pregunta antes que a la base de datos, y que puede no estar.

```c
struct CacheDriver {
  const char* cd_name;
  void (*cd_get)(cache_id_t id, const char* key);
  void (*cd_set)(cache_id_t id, const char* key, const char* value, int ttl);
  void (*cd_del)(cache_id_t id, const char* key);
  void (*cd_cancel)(cache_id_t id);
};
```

Los valores son cadenas para el core y JSON para quien las escribe y las lee:
jansson ya está en el árbol como dependencia del driver de PostgreSQL, y
`db.h` ya usa el truco de declarar `struct json_t` sin definirlo. Aquí ni eso
hace falta: el core pasa la cadena tal cual.

**El bloque va en `ircd.conf`**, junto al de la base de datos, porque la
conexión es del servidor y no de un módulo:

```
Redis {
  host = "127.0.0.1";
  port = 6379;
  password = "${REDIS_PASSWORD}";
  database = 0;
  pool = 2;
  timeout = 200;          # milisegundos; una caché lenta no es una caché
  prefix = "ircu:";
};
```

---

## 4. El protocolo

### 4.1 SASL, para el cliente que lo negocia

Lo estándar de IRCv3:

```
  C: CAP LS 302
  S: CAP * LS :... sasl=PLAIN,EXTERNAL ...
  C: CAP REQ :sasl
  S: CAP * ACK :sasl
  C: AUTHENTICATE PLAIN
  S: AUTHENTICATE +
  C: AUTHENTICATE bWFyaWEAbWFyaWFAZWplbXBsby5jb20Ac2VjcmV0bw==
  S: 900 maria maria!u@host maria :You are now logged in as maria
  S: 903 maria :SASL authentication successful
  C: CAP END
```

- **`PLAIN`** es `authzid \0 authcid \0 contraseña`: el authcid es el **email**
  y el authzid es **cuál de sus cuentas** se quiere usar; vacío, la marcada por
  defecto. Ese campo existe exactamente para eso.
- **`EXTERNAL`**: la credencial es el certificado de cliente y lo que el
  proveedor recibe es la huella que el core ya calcula. El authzid sigue
  eligiendo cuenta.
- Numerics 900-908, que `numeric.h` dejó reservados para esto.
- Trozos de 400 bytes, `+` vacío, `*` aborta, tope `SASL_MESSAGE_MAX`.

**Lo asíncrono se resuelve como el registro, porque es el registro.** Entre
`CAP END` y el `001` puede haber una consulta en marcha. El registro ya es una
máquina de estados que espera por ident, DNS, CAP, la cookie de PING y iauth;
SASL es una bandera más: `AR_SASL_PENDING`, con `auth_sasl_start()` y
`auth_sasl_done()` calcados de `auth_cap_start()` y `auth_cap_done()`. Mismo
razonamiento que llevó `HOOK_CLIENT_PRE_REGISTER` a `s_auth.c` en la fase 0.

### 4.2 `ACCOUNT`, para todos los demás

```
  ACCOUNT LOGIN <email> <contraseña> [<nick>]
  ACCOUNT LOGOUT
  ACCOUNT LIST
```

**Sin token P10**, por la razón de `AUTHENTICATE` (§2) y por una más: `AC`
es lo que usaba el burst de cuentas del ircu histórico, y darle aquí otro
significado haría que el burst de un par antiguo cayera en un comando de
cliente. Nada de `ACCOUNT` cruza un enlace.

`LOGIN` funciona **antes y después** de completar la conexión: antes se
comporta como `PASS` y retiene el registro con la misma `AR_SASL_PENDING`;
después identifica o cambia de cuenta activa.

La credencial se entrega por `sasl_login_request()`, que es por donde entran
también las de `AUTHENTICATE`: la pregunta es la misma y lo que se hace con
la respuesta —el `+r`, el nick, la retención del registro— también, así que
hay un solo camino y no dos que haya que mantener sincronizados. Lo único
que decide el comando es en qué numerics se le habla al cliente: quien nunca
negoció la capacidad `sasl` no oye hablar de SASL. Los fallos van en un
`ERR_ACCOUNTFAIL` (983) con el motivo; el éxito, en el 900 de siempre; el
`LOGOUT`, en el 901 con el `guest-*` que ya lleva puesto.

El tope de intentos es de **fallos seguidos**, no de intentos: un `LOGIN`
correcto lo pone a cero, porque cambiar de cuenta es identificarse y quien
acaba de demostrar quién es no ha atacado nada.

**`LIST`** enumera las cuentas del email con el que estás autenticado, marcando
la que estás usando y la que es la de por defecto:

```
  :servidor 984 maria maria * :en uso
  :servidor 984 maria maria_movil - :disponible
  :servidor 984 maria mrodriguez d :por defecto
  :servidor 985 maria :Fin de ACCOUNT LIST
```

El segundo parámetro son banderas para el cliente —`*` la que está en uso,
`d` la de por defecto, `-` ninguna— y el texto es para la persona, y se
traduce. Cuál está en uso lo sabe el servidor, no el proveedor: el proveedor
sabe qué cuentas tiene la dirección, el servidor sabe cuál lleva puesta el
cliente.

**`LIST` exige estar autenticado**, y no por prudencia: el dato de entrada es
`cli_user(sptr)->email`, que sólo existe si hubo un `LOGIN`. Sin él la consulta
no tiene con qué hacerse, y preguntar «dame las cuentas de este email» a quien
no ha probado que el email es suyo es un enumerador de cuentas. Sin
autenticar, `ERR_NOTAUTHENTICATED` (986).

**`ACCOUNT` tiene exactamente `LOGIN`, `LOGOUT` y `LIST`, y no crece.**
Registrar una cuenta, verificar un email o cambiar una contraseña no tocan
`struct Client`: son política, y `nickserv` los atiende por `/msg` o con
comandos propios (`module_add_command()`).

**Ni `ACCOUNT LOGIN` ni `PLAIN` funcionan sin TLS**, salvo que
`FEAT_ACCOUNT_REQUIRE_TLS` se ponga a cero a sabiendas: las dos mandan la
contraseña en claro.

### 4.3 El cliente tradicional

Requisito de aceptación, no aspiración:

- Un cliente que no pide `sasl` no ve `AUTHENTICATE` jamás.
- `ACCOUNT` es un comando más; quien no lo escribe no lo nota.
- `+r` ya existía; `+f` es un modo más en el `MODE` que ya recibía.
- Sigue sin haber `account-notify`, `account-tag` ni `extended-join`, y ahora
  por una razón mejor que «se quitaron»: **con una cuenta que es el nick, las
  tres son redundantes.** `account-tag` pondría `@account=maria` en una línea
  cuyo prefijo ya dice `maria!u@host`.

---

## 5. La regla nick ↔ cuenta, y el `guest-*`

Consecuencia directa de «una cuenta es un nick». Entera, porque aquí es donde
se esconden los casos raros.

1. **Identificarse implica llevar el nick de la cuenta.** No existe
   «identificado como `maria` pero llamándose `pedro`». Si existiera, `+r`
   dejaría de significar lo que significa.

2. **Al autenticar durante el registro**, si el nick elegido no es el de la
   cuenta, el servidor lo cambia antes de completar el registro; el cliente se
   entera por el `900`, que lleva el nick final.

3. **Si la autenticación falla, o el nick de la cuenta está ocupado, el cliente
   se llama `guest-<aleatorio>`.** No entra con el nick que pidió: pedir un
   nick registrado y no poder probarlo es indistinguible de intentar
   suplantarlo, y dejarle el nombre «mientras tanto» es dejarle exactamente lo
   que quería. El sufijo son ocho caracteres base 62 de `ircrandom()`, así que
   hay 2×10¹⁴ nombres; **si aun así está tomado, el cliente se lleva un KILL**
   en vez de un segundo intento, porque un bucle de reintentos ante algo que
   nunca ocurre es código que nadie va a probar jamás y que un día se ejecuta.

4. **`ACCOUNT LOGOUT` también renombra a `guest-*`.** Salir de la cuenta es
   perder `+r`, y quedarse con el nick de la cuenta sin `+r` es exactamente el
   estado que el modelo no admite: el siguiente en mirar vería a `maria` sin
   identificar y no sabría si es un cierre de sesión o una suplantación. El
   nick se suelta con la sesión.

5. **Cambiar de nick deja de estar identificado.** Ya es así y no se toca:
   `set_nick_name()` limpia `+r` en cada servidor sin mandar nada por el
   enlace, y un cambio sólo de mayúsculas conserva la identificación.

6. **El prefijo es `FEAT_GUEST_PREFIX`** (por omisión `guest-`), y el servidor
   comprueba al arrancar que el prefijo más el sufijo caben en `NICKLEN`.

---

## 6. Congelar: el modo `+f`

Un cliente sin identificar que lleva un nick registrado queda **congelado**
mientras corre el plazo para autenticarse. No es una cortesía: durante ese
plazo está usando un nombre que no ha probado que sea suyo, y todo lo que haga
—entrar a un canal, mandar un mensaje, cambiar un modo— lo hace bajo la
identidad de otro.

**`+f` (freeze) es un modo de usuario del core**, en el bit que le corresponde
a la letra (31, `f`), global como los demás, y **sólo un servidor o un bot de
servicio puede ponerlo o quitarlo** — igual que `+B` y `+S`:
`set_user_mode()` deshace las dos direcciones para cualquier cliente local,
operadores incluidos. `bot_set_user_mode()` gana `+f`/`-f` junto a `+r`/`-r`,
que es por donde `nickserv` lo aplica.

**Aparece en `WHOIS`** con su propio numeric, para que se vea desde fuera por
qué ese usuario no contesta.

**Qué puede hacer un cliente congelado**, y nada más:

- autenticarse: `AUTHENTICATE`, `ACCOUNT`;
- hablar con un servicio: `PRIVMSG` y `NOTICE` **cuyo destino sea un bot `+S`**
  — que es lo que hace falta para `/msg NickServ identify`;
- cambiar de nick: `NICK`, que es una de las salidas de la congelación;
- lo que sostiene la conexión: `CAP`, `PING`, `PONG`, `QUIT`.

Todo lo demás recibe `ERR_FROZEN` (987). **La lista no se escribe en
`parse.c`**: cada comando declara en `msgtab[]` si sobrevive a la congelación,
con una bandera nueva `MFLG_FROZEN_OK`, de la misma forma que ya declara
`MFLG_UNREG` y su sujeto. Un comando que un módulo registre puede declararlo
igual. La puerta está en `parse_dispatch()`, que es el único sitio por el que
pasan los dos caminos de despacho y ya tiene el `parv` desmenuzado.

**Qué levanta la congelación** — las tres cosas, y ninguna más:

- el usuario se autentica y se queda con el nick de su cuenta;
- vence el plazo y `nickserv` lo renombra a `guest-*`;
- el usuario cambia por su cuenta a un nick que no está registrado.

**Y una cuarta, que es una salvaguarda y no una regla:** si el proveedor de
identidad desaparece —módulo descargado, sin driver—, nadie puede ya
descongelar a nadie. El core **renombra a `guest-*`** a todos los congelados en
lugar de descongelarlos donde están: descongelar sería dejar a un posible
impostor con el nick y sin nadie mirando, que es justo lo que §7 prohíbe.

---

## 7. Si no se puede comprobar, no se puede usar

**Corrección 3, y manda sobre cualquier otra consideración de este documento.**

Cuando el servidor no puede averiguar de quién es un nick —PostgreSQL caído,
tiempo de espera agotado, proveedor sin registrar—, ese nick **no se puede
usar**. No hay modo degradado en el que se entre sin `+r` y ya se verá: el
único momento en que un impostor consigue el nick de otro es precisamente
cuando el servicio que lo protege no está mirando, y un atacante puede provocar
ese momento.

En la práctica:

- **Al registrarse**: si la consulta falla, el cliente entra como `guest-*`.
  Entra, que es lo importante —la red no se cierra porque se caiga una base de
  datos— pero no con un nombre que nadie ha podido verificar.
- **Al cambiar de nick**: si la consulta falla, el cambio se rechaza y el
  cliente se queda como estaba. Quedarse con el nick que ya tenía no concede
  nada nuevo.
- **Redis caído no es esto.** Redis es una caché: si no está, se pregunta a
  PostgreSQL y todo funciona más despacio. Lo que dispara esta regla es no
  tener respuesta de la fuente de verdad (§10).

La contrapartida honesta: una base de datos caída convierte a todo el mundo en
`guest-*`. Es la decisión tomada, y es la correcta — un rato de nicks feos se
arregla solo, una suplantación no.

---

## 8. El email en `struct User`

`struct User` gana un miembro:

```c
  /** Email de la identidad con la que este usuario se autenticó, o NULL.
   *
   * Local y sólo local: no cruza P10, no aparece en ningún numeric que
   * vaya a un tercero, y lo pone el servidor que autenticó.  Un usuario
   * que llega de otro servidor con +r no lo trae, y no se le inventa.
   */
  const char* email;
```

Cuatro reglas, y cada una con su prueba:

1. **Sólo lo ve su dueño.** En `WHOIS` sale con `RPL_WHOISEMAIL` (691) y bajo
   `acptr == sptr`, la misma condición que ya gobierna `RPL_WHOISACTUALLY` y
   `RPL_WHOISLANGUAGE`. **Sin excepción para operadores**: un operador necesita
   la IP para moderar, no el correo de nadie.
2. **`LOGOUT` lo borra**, y salir del cliente también. Es el mismo instante en
   que se pierde `+r`: mientras uno de los dos exista sin el otro, hay un
   estado que el modelo no define.
3. **No cruza el enlace.** Un usuario remoto tiene `email == NULL` siempre. Se
   comprueba con una prueba de integración entre dos servidores.
4. **No va a `WHOWAS`** ni a ningún sitio que sobreviva a la conexión.

Que esté en `struct User` y no en `struct Connection` es deliberado: un usuario
remoto tiene `User` y no tiene `Connection`, así que el campo existe para él y
vale `NULL` — que es justo la afirmación «este servidor no sabe su email», y es
más fácil de leer que un campo que no existe.

---

## 9. Los módulos

### 9.1 `identity` — el mecanismo

`modules/services/identity/`, de tipo directorio porque lleva migraciones. No
tiene bot, no registra comandos y no manda un solo mensaje a un usuario.
Registra el proveedor de §3.2 y contesta.

```sql
CREATE TABLE identity (              -- el email
  id            BIGSERIAL PRIMARY KEY,
  email         TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,       -- $argon2id$..., los costes dentro
  mfa_secret    BYTEA,               -- AES-256-GCM
  sso_subject   TEXT UNIQUE,
  verified_at   TIMESTAMPTZ,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE account (               -- el nick
  id           BIGSERIAL PRIMARY KEY,
  identity_id  BIGINT NOT NULL REFERENCES identity(id) ON DELETE CASCADE,
  nick         TEXT NOT NULL,        -- como lo escribió quien lo registró
  nick_canon   TEXT NOT NULL UNIQUE, -- normalizado como lo normaliza el ircd
  is_default   BOOLEAN NOT NULL DEFAULT false,
  suspended_at TIMESTAMPTZ,
  created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

**`nick_canon` se normaliza como lo hace el ircd**, no como lo haría
PostgreSQL: en IRC `[`, `]` y `\` son las mayúsculas de `{`, `}` y `|`
(`ircd_strcmp`), así que `lower()` daría dos cuentas donde la red ve un solo
nick. La normalización la hace el módulo antes de escribir.

**Todo alta lleva un lock, y el `UNIQUE` es el refuerzo, no la defensa.**
Contar las cuentas de un email y luego insertar es una carrera en cuanto hay
dos servidores: los dos cuentan dos, los dos insertan, el email acaba con
cuatro. La transacción empieza tomando un cerrojo consultivo sobre el email, y
otro sobre el nick canónico:

```sql
BEGIN;
  SELECT pg_advisory_xact_lock(hashtext('ident:' || $1));   -- el email
  SELECT pg_advisory_xact_lock(hashtext('nick:'  || $2));   -- el nick
  -- contar, comprobar el límite, insertar
COMMIT;
```

El cerrojo es de transacción (`_xact_`) a propósito: se suelta solo con el
`COMMIT` o el `ROLLBACK`, así que un servidor que se muera a mitad no deja un
email bloqueado para siempre. El `UNIQUE` sigue estando para lo que el cerrojo
no cubre — alguien insertando a mano.

**La verificación no bloquea.** `ap_verify()` lanza la consulta y vuelve;
cuando llegan las filas, **el Argon2 va a un `worker`** —`ircd_pwhash_verify()`
es puro justamente para esto— y el resultado se entrega con
`account_complete()` desde el hilo principal. Dos saltos, ninguna espera: la
cadena que la fase 0 dejó montada, usada por fin de punta a punta.
`ircd_pwhash_outdated()` decide si rehacer el hash con los costes actuales; se
rehace en el mismo `worker`, **después** de contestar, para no sumar latencia a
la conexión.

### 9.2 `nickserv` — la política y la voz

`modules/services/nickserv/`. Tiene el bot (`Service{}` + `bot_create()`),
escucha `HOOK_CLIENT_REGISTERED` y `HOOK_CLIENT_NICK_CHANGED`, pregunta a
`identity` de quién es el nick, y aplica el plazo: avisa, pone `+f`, arma un
temporizador, y al vencer renombra a `guest-*`. Atiende `/msg NickServ` para
registrar, identificarse, verificar el correo y cambiar la contraseña.

**Su configuración va en el bloque `Service{}` que ya lo declara**, no en uno
propio. Un `NickServ{}` aparte sería un segundo sitio donde se escribe lo mismo
—el nick del bot está ya en `Service{}`— y una palabra reservada nueva en el
léxico por cada servicio que aparezca:

```
Service {
  name = "NickServ";
  type = "nickserv";
  channel = "#servicios";

  "max_accounts" = 3;          # cuántas cuentas agrupa un email
  "grace_period" = 1 minutes;  # antes de renombrar a guest-*
};
```

Una opción es un nombre entrecomillado y un valor; el core los guarda
literales y no lee ninguno, y el módulo que implementa el tipo lee los que
conoce con `conf_service_option()` / `conf_service_option_int()`. El lado
izquierdo es una cadena, así que ninguna opción puede chocar con `name`,
`type` o `channel`, y se acepta un número o un `timespec` igual que una
cadena porque la mayoría son números. Un módulo de servicio encuentra su
bloque con `conf_find_service_type()`: conoce el tipo que implementa, no el
nick que un operador le puso.

**El límite de cuentas por email vive ahí y no en el core**, porque quien lo
administra es el servicio: subirlo es una decisión de producto, y el core no
tiene por qué enterarse. Tres es el valor de partida, no una constante.

Con esto **`nickserv` ya no depende de `module_add_config_block()`** (006 §5.6),
que era el único motivo por el que hacía falta en esta fase.

### 9.3 `redis` — el driver de caché

`modules/workers/redis/`, con hiredis y jansson, y su fragmento
`module.cmake` calcado del de `postgres`: si hiredis no está, el módulo no se
construye y el resto del árbol no se entera. Un worker dedicado por conexión
del pool, como el driver de PostgreSQL, y por la misma razón: hiredis bloquea y
el hilo principal no puede.

---

## 10. Primero Redis, luego PostgreSQL

Cada consulta de «¿de quién es este nick?» ocurre en cada registro y en cada
cambio de nick de toda la red. Sin caché, eso es la base de datos en el camino
crítico de conectarse.

```
  ¿de quién es "maria"?
        │
        ├─► Redis  GET ircu:nick:maria
        │     ├─ acierto  → contestar, fin
        │     └─ fallo    ↓
        ├─► PostgreSQL SELECT ... WHERE nick_canon = 'maria'
        │     ├─ fila     → SETEX ircu:nick:maria <ttl> {...}  → contestar
        │     ├─ nada     → SETEX ircu:nick:maria <ttl> {"free":true}
        │     └─ error    → NO SE PUEDE USAR EL NICK (§7)
```

Tres cosas que hay que hacer bien o la caché hace daño:

1. **Cachear también los fallos.** «Este nick no está registrado» es la
   respuesta mayoritaria, y si no se cachea, la mayoría del tráfico llega a
   PostgreSQL de todas formas y la caché no sirve para lo que se puso.
2. **Invalidar al escribir, no esperar al TTL.** Registrar, borrar o suspender
   una cuenta borra su clave; Redis es compartido, así que borrar en un nodo
   vale para todos. El TTL es la red de seguridad de lo que se escriba fuera
   del ircd, no el mecanismo.
3. **Redis caído no es un error, es lentitud.** Se va a PostgreSQL y ya. Lo que
   dispara §7 es que falle la fuente de verdad, no la caché.

Los valores son JSON, con jansson, porque el registro de un nick tiene más de
un campo —identidad, si está suspendido, si es la de por defecto— y meterlo en
una cadena con separadores es inventar un formato que habrá que versionar.

---

## 11. Orden de implementación

Cada punto compila, pasa pruebas y se sube por separado.

1. **`sasl.c` + `sasl.h`** — el registro de mecanismos, `PLAIN` y `EXTERNAL`.
   *(hecho)*
2. **El estado en el core** *(hecho)* — `User::email` y su `WHOIS`, el modo `+f`,
   `MFLG_FROZEN_OK` y la puerta en `parse_dispatch()`, los numerics.
   Comprobable sin proveedor: se pone `+f` a mano desde un servidor.
3. **`account.c` + `account.h`** *(hecho)* — el registro de proveedor, las peticiones en
   vuelo, `account_login()` / `account_logout()` y el renombrado a `guest-*`.
4. **`m_authenticate.c` + `AR_SASL_PENDING`** *(hecho)* — el diálogo y la retención del
   registro. Con un proveedor de pruebas ya se autentica de punta a punta.
5. **`m_account.c`** *(hecho)* — `LOGIN`, `LOGOUT` y `LIST`, sin token P10
   (ver §4.2), sobre `sasl_login_request()`: la misma pregunta que hace
   `AUTHENTICATE` y el mismo camino de la respuesta a `+r`.
6. **`cache.c` + `cache.h` + `modules/workers/redis/`** — el tercer registro y
   su driver, con el bloque `Redis{}`. *(hecho; ver `doc/readme.cache`)*
7. **`modules/services/identity/`** — el almacén, las migraciones con sus
   cerrojos, Redis delante, el Argon2 en un `worker`.
8. **`modules/services/nickserv/`** — el bot, el plazo de gracia, y sus
   opciones dentro del `Service{}` que ya lo declara.
9. **Documentación y pruebas** — `doc/readme.accounting` reescrito entero,
   `doc/readme.sasl`, y las de integración: que el email no cruza el enlace,
   que un congelado no puede hacer nada, que el `guest-*` ocurre en los cuatro
   sitios en que debe ocurrir.

---

## 12. Lo que esta fase no hace

- **No** mete la comprobación de credenciales en el core.
- **No** enruta SASL entre servidores: no hace falta (§2).
- **No** reintroduce `account-notify`, `account-tag` ni `extended-join` (§4.3).
- **No** añade un nombre de visualización libre. El nombre visible es el nick.
- **No** veta nicks para protegerlos: congela y luego renombra (§6).
- **No** hace 2FA ni SSO todavía: las columnas están para no migrar dos veces,
  pero los mecanismos que las usan son módulos posteriores, y el registro de
  §3.1 existe para que quepan sin tocar el core.

---

## 13. Riesgos

- **Una base de datos caída convierte a la red en `guest-*`.** Es la
  consecuencia aceptada de §7, y hay que decirla en voz alta porque se va a
  notar el día que pase.
- **`+f` es una puerta nueva en el despacho de comandos.** Un fallo ahí no
  congela a nadie de más, congela a todos: la puerta se prueba con un caso por
  cada comando de la lista blanca y uno por cada categoría de fuera.
- **Redis añade un sitio donde la verdad puede quedarse vieja.** Mitigado
  invalidando al escribir (§10.2); el TTL es el suelo, no el plan.
- **Descargar `identity` renombra a todo el mundo que esté congelado.** Es lo
  correcto (§6) y es ruidoso; conviene que `/MODULE UNLOAD identity` lo diga
  antes de hacerlo.
- **`ACCOUNT LOGIN` en claro.** Mitigado exigiendo TLS por omisión, y la
  mitigación real es que exista `EXTERNAL`.
- **El nick como identidad limita el producto**, y ya se dijo en la 006 §6.2.
  Esta propuesta lo asume, no lo resuelve.
