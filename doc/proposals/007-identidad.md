# Propuesta 007 — Identidad: SASL, `ACCOUNT` y el modelo de cuentas

**Estado:** propuesta, para aprobar.
**Es la fase 1 de la 006** (§6), que dejó tomadas las decisiones de fondo; esto
las convierte en un diseño con el que se puede escribir código.
**Depende de:** 001 (API de módulos), 006 §5.1 (capacidades dinámicas),
§5.5 (hooks asíncronos), §5.8 (criptografía `ircd_*`), y de `db.h` +
`migration.h` para el módulo.
**Introduce:** `include/sasl.h` + `ircd/sasl.c`, `include/account.h` +
`ircd/account.c`, `ircd/m_authenticate.c`, `ircd/m_account.c`,
`modules/services/identity/`.
**Revierte:** la retirada de SASL y de las cuentas documentada en
`doc/readme.accounting`, que se reescribe entera.

---

## 0. Lo que ya está decidido

De la 006 §6, y no se vuelve a discutir aquí:

1. **SASL y `ACCOUNT` vuelven, y van al core**, porque tocan `struct Client`.
   Sin la filosofía de Undernet.
2. **La credencial es el email. Una cuenta *es* un nickname, sin excepción. Un
   email agrupa como máximo tres cuentas.**
3. **La comprobación de la credencial no entra en el core.** Es política, vive
   en un módulo, y se resuelve con el hook asíncrono y el hash en un *worker*.
4. **El email no se publica nunca**: ni en `WHOIS`, ni en un numeric, ni en P10.
5. **Se mantiene la compatibilidad con los clientes tradicionales**, como en
   toda la 006: quien no negocia nada recibe lo de siempre, byte a byte.

Lo que esta propuesta decide es *cómo*.

---

## 1. El reparto: qué es protocolo y qué es política

La línea es mecánica, no estética: **el core habla el protocolo y toca
`struct Client`; el módulo decide si la credencial es correcta.** Nada más
cruza.

| Core | Módulo `identity` |
| --- | --- |
| El comando `AUTHENTICATE` y la máquina de estados de SASL | Qué es un email válido |
| El registro de mecanismos, con `PLAIN` y `EXTERNAL` dentro | `SCRAM-SHA-256`, `OAUTHBEARER`, lo que venga |
| La capacidad `sasl` y su valor | — |
| El comando `ACCOUNT LOGIN` / `LOGOUT` | Registrar, verificar, cambiar contraseña |
| Conceder y quitar `+r`, y propagarlo | El límite de tres cuentas por email |
| Retener el registro mientras se comprueba | La consulta, el Argon2, el `identity`/`account` en la base |
| Que el email no salga del proceso | Guardar el email |

La razón de que el mecanismo esté en el core y la comprobación no: el mecanismo
es la forma del diálogo con el cliente —cuántas vueltas, qué se codifica en
base64, qué numeric se manda— y eso es idéntico en cualquier despliegue. Si la
credencial es correcta depende de una base de datos que el core no debe conocer,
y de reglas —caducidad, 2FA, SSO, suspensiones— que cambian por organización.

---

## 2. Lo que no cruza P10, y por qué eso simplifica todo

**`AUTHENTICATE` no cruza el enlace. `SASL` no es un token P10. No hay servidor
de servicios al que enrutar la autenticación.**

En una red IRC clásica esto no es así: SASL se relaya a un servicio central
porque sólo él tiene las credenciales. Aquí el módulo `identity` corre en cada
servidor y consulta la **misma base de datos** (el bloque `Database{}` y el
driver `postgres`, que ya existen), de modo que cada servidor puede comprobar
por sí mismo. Lo que se elimina con eso:

- el enrutado de SASL entre servidores y su tabla de sesiones a medias;
- el modo degradado cuando el servicio está partido de la red;
- un token P10 nuevo por cada vuelta del diálogo.

**Lo único que viaja es `+r`, que ya viaja.** Como una cuenta *es* un nick, el
modo `+r` sobre un cliente que se llama `maria` dice, sin parámetros y sin
ambigüedad, «este `maria` es el `maria` de verdad». Eso ya está implementado,
ya cruza en el burst y ya se limpia al cambiar de nick
(`doc/readme.accounting`). **No hace falta protocolo nuevo entre servidores.**

Consecuencia que conviene anotar: el módulo `identity` y el driver de base de
datos deben estar cargados en **todos** los servidores de la red. Un servidor
sin ellos no puede autenticar a nadie, pero sigue viendo y respetando el `+r`
que le llega de los demás. Degrada bien.

---

## 3. Dos registros en el core

Ambos con la forma que el árbol ya usa dos veces —los modos por módulo
(`client.c`, `chan_modes.c`), las capacidades (`capab.c`) y el driver de base
de datos (`db.c`)—, por la misma razón: un módulo no puede resolver los
símbolos de otro (`RTLD_LOCAL`), así que el punto de encuentro lo tiene el core.

### 3.1 Mecanismos SASL — `include/sasl.h`, `ircd/sasl.c`

Una lista en tiempo de ejecución, como `capab.c`. El core registra `PLAIN` y
`EXTERNAL` al arrancar; un módulo añade los suyos con
`module_add_sasl_mechanism()` y se van con él al descargarse.

```c
struct SaslMechanism {
  const char* sm_name;        /* "PLAIN", en mayúsculas */
  unsigned int sm_flags;      /* SASL_MECH_NEEDS_TLS, ... */
  /* Una vuelta del diálogo.  Devuelve SASL_CONTINUE con un reto,
   * SASL_CREDENTIAL con lo que hay que comprobar, o SASL_FAIL. */
  enum SaslStep (*sm_step)(struct SaslSession* ses, const char* in,
                           size_t inlen);
};
```

`sasl.c` no conoce clientes ni sockets: recibe los bytes ya decodificados y
devuelve o bien un reto o bien una credencial. Eso es lo que permite probarlo
sin servidor (`sasl_t`), igual que `migration.c` se prueba sin base de datos y
`capab.c` sin cliente. El pegamento con el cliente está en `m_authenticate.c`.

**El valor de la capacidad se recalcula solo.** `sasl=PLAIN,EXTERNAL` sale de
recorrer el registro; cuando un módulo añade un mecanismo, `cap_set_value()` lo
actualiza y quien negocie `cap-notify` recibe el `CAP NEW`. Eso ya funciona
desde la fase 0 y no hay que escribirlo otra vez.

**Y la capacidad sólo se ofrece si hay quien conteste.** Sin proveedor
registrado (§3.2), `cap_update_availability()` retira `sasl` del `CAP LS`. Un
cliente que negocia SASL y se encuentra con que el servidor no puede
autenticar a nadie es peor que un cliente que ve que no se ofrece.

### 3.2 Proveedor de identidad — `include/account.h`, `ircd/account.c`

Un solo proveedor a la vez, registrado por un módulo, con la forma exacta de
`db_register_driver()` y por los dos mismos motivos: el core es el punto de
encuentro, y es el core quien sostiene las peticiones en vuelo para que el
módulo se pueda descargar con alguna a medias.

```c
struct AccountProvider {
  const char* ap_name;
  /* Comprobar una credencial.  El proveedor copia lo que necesite de
   * *req -- que es del core y no sobrevive a la llamada -- y contesta
   * más tarde con account_complete(id, ...), en el hilo principal. */
  void (*ap_verify)(account_id_t id, const struct AccountRequest* req);
  void (*ap_cancel)(account_id_t id);
};
```

`struct AccountRequest` lleva el mecanismo, el *authcid* (el email), el
*authzid* (el nick que se quiere usar, o vacío), el secreto, la huella TLS del
cliente y su numnick. **El secreto se borra con `ircd_crypto_wipe()` en cuanto
el proveedor lo ha copiado**, y el proveedor está obligado a lo mismo.

`account_complete(id, resultado, nick, motivo)` devuelve **sólo el nick**. El
email no vuelve. No es una comodidad: es el punto donde se impone §0.4, y lo
impone la firma de la función, no la buena voluntad de quien escriba el módulo.

Descargar el módulo falla toda petición en vuelo con `ACCOUNT_ERR_UNAVAILABLE`
antes de volver, como hace `db_unregister_driver()`.

---

## 4. El protocolo

### 4.1 SASL, para el cliente que lo negocia

Lo estándar de IRCv3, sin invenciones:

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

- **`PLAIN`** es `authzid \0 authcid \0 contraseña`. Aquí el *authcid* es el
  **email** y el *authzid* es **el nick de cuál de las tres cuentas** se quiere
  usar; vacío significa la marcada por defecto. Ese campo existe exactamente
  para esto, así que no hay que inventar sintaxis.
- **`EXTERNAL`** no lleva secreto: la credencial es el certificado de cliente,
  y lo que el proveedor recibe es la huella que el core ya calcula
  (`cli_tls_fingerprint()`). El *authzid* sigue eligiendo cuenta.
- Numerics 900-908, que la 006 dejó reservados en `numeric.h` precisamente
  para esto.
- Trozos de 400 bytes, `+` para vacío, `*` para abortar, tope de
  `FEAT_SASL_MAX_LENGTH` en total.

**Lo asíncrono se resuelve como el registro, porque es el registro.** Entre
`CAP END` y el `001` puede haber una consulta a la base de datos en marcha. El
registro ya es una máquina de estados que espera por ident, DNS, CAP, la cookie
de PING y iauth; SASL es una bandera más al lado de esas: `AR_SASL_PENDING`,
con `auth_sasl_start()` y `auth_sasl_done()` calcados de `auth_cap_start()` y
`auth_cap_done()`. Es el mismo razonamiento que llevó `HOOK_CLIENT_PRE_REGISTER`
a `s_auth.c` en la fase 0, y por eso no hace falta mecanismo nuevo.

### 4.2 `ACCOUNT`, para todos los demás

```
  ACCOUNT LOGIN <email> <contraseña> [<nick>]
  ACCOUNT LOGOUT
```

Token P10 `AC`, libre. Funciona **antes y después** de completar la conexión,
que es lo que se pidió: antes se comporta como `PASS` y retiene el registro con
la misma `AR_SASL_PENDING`; después identifica, cambia de cuenta activa o
cierra la sesión.

**`ACCOUNT` tiene exactamente `LOGIN` y `LOGOUT`, y no crece.** Registrar una
cuenta, verificar un email o cambiar una contraseña no tocan `struct Client`:
son política, y el módulo registra sus propios comandos para eso
(`module_add_command()`). La regla que separa lo uno de lo otro es la misma de
§1, aplicada a los subcomandos.

**Ni `ACCOUNT LOGIN` ni `PLAIN` funcionan sin TLS**, salvo que
`FEAT_ACCOUNT_REQUIRE_TLS` se ponga a cero a sabiendas. Ambos mandan la
contraseña en claro; el propio RFC de SASL lo dice de `PLAIN`, y un servidor que
lo permite por omisión está entregando contraseñas. Por omisión: exigido.

### 4.3 El cliente tradicional

No cambia nada para él, y esto es requisito de aceptación, no aspiración:

- Un cliente que no pide `sasl` no ve `AUTHENTICATE` jamás.
- `ACCOUNT` es un comando más: quien no lo escribe no lo nota.
- `+r` ya existía y ya se mostraba igual.
- Sigue sin haber `account-notify`, `account-tag` ni `extended-join`, y ahora
  por una razón mejor que «se quitaron»: **con una cuenta que *es* el nick, las
  tres son redundantes.** `account-tag` pondría `@account=maria` en una línea
  cuyo prefijo ya dice `maria!u@host`; `account-notify` anunciaría un cambio de
  cuenta que el cliente ya ve como el `NICK` que es. Reintroducirlas sería
  gastar ancho de banda en repetir el prefijo.

---

## 5. La regla nick ↔ cuenta

Es la consecuencia directa de «una cuenta es un nick» y hay que escribirla
entera, porque es donde se esconden los casos raros.

1. **Identificarse implica llevar el nick de la cuenta.** No hay estado
   «identificado como `maria` pero llamándose `pedro`». Si lo hubiera, `+r`
   dejaría de significar lo que significa y volveríamos a la pregunta que
   §6.2 de la 006 quitó de en medio.

2. **Al autenticar durante el registro**, si el nick elegido no es el de la
   cuenta, el servidor lo cambia antes de completar el registro. El cliente se
   entera por el `900`, que lleva el nick final. Si ese nick está ocupado por
   otro, la autenticación falla con `902` (`ERR_NICKLOCKED`) y el cliente entra
   sin identificar o se va; no se le deja a medias.

3. **Al autenticar ya conectado**, el cambio de nick lo hace el servidor como
   parte del `LOGIN`, con el `NICK` que la red ya sabe interpretar. Si está
   ocupado, el `LOGIN` falla y no se toca nada. Conceder `+r` y cambiar el nick
   son lo mismo y ocurren juntos o no ocurren.

4. **Cambiar de nick deja de estar identificado.** Ya es así hoy y no se toca:
   `set_nick_name()` limpia `+r` en cada servidor sin mandar nada por el
   enlace, y un cambio sólo de mayúsculas conserva la identificación con la
   cuenta reescrita.

5. **`LOGOUT` quita `+r` y deja el nick donde está.** El nick sigue siendo
   suyo hasta que lo suelte; lo que se pierde es la prueba.

---

## 6. Proteger un nick registrado: por plazo, no por veto

Si las cuentas son nicks, alguien acabará usando el nick de otro. La política
—cuánto se tolera, qué se hace— es del módulo. El **mecanismo** es la decisión
de esta propuesta, y es la de siempre en IRC: **plazo de gracia, no veto.**

El módulo escucha `HOOK_CLIENT_REGISTERED` y `HOOK_CLIENT_NICK_CHANGED`,
consulta si ese nick tiene cuenta, y si la tiene y el cliente no está
identificado le avisa y arma un temporizador; al vencer, le cambia el nick a
uno de invitado. No se veta.

La alternativa —vetar el nick hasta saber si está registrado— exigiría
suspender `HOOK_CLIENT_PRE_NICK`, y eso la fase 0 ya lo descartó con el
razonamiento que quedó en `hooks.h`: un comando se despacha, se maneja y se
termina antes de leer la línea siguiente, y volver a entrar en un manejador a
medias no es algo que el servidor sepa hacer. El plazo de gracia no necesita
nada nuevo, es lo que los usuarios de IRC ya esperan, y falla del lado seguro:
si la base de datos no contesta, nadie pierde su nick.

`HOOK_CLIENT_PRE_REGISTER` **sí** es suspendible, así que el caso de conectar
con un nick registrado se puede resolver limpiamente antes de que el cliente
entre. Los dos caminos conviven.

---

## 7. El módulo `identity`

`modules/services/identity/`, de tipo directorio porque lleva migraciones.

### 7.1 Los datos

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
  id          BIGSERIAL PRIMARY KEY,
  identity_id BIGINT NOT NULL REFERENCES identity(id) ON DELETE CASCADE,
  nick        TEXT NOT NULL UNIQUE,  -- comparado como lo compara el ircd
  is_default  BOOLEAN NOT NULL DEFAULT false,
  suspended_at TIMESTAMPTZ,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

Dos cosas que hay que resolver en SQL y no en C:

- **El límite de tres** es un disparador o una restricción, no un `SELECT
  count(*)` seguido de un `INSERT`: dos registros a la vez en dos servidores
  distintos se cuelan por esa rendija.
- **La comparación de nicks** tiene que ser la del ircd (`ircd_strcmp`, con
  `[]\` equivalentes a `{}|`), no la de PostgreSQL. Se guarda una columna
  normalizada con la forma canónica del ircd y el `UNIQUE` va sobre ella.

### 7.2 La verificación

`ap_verify()` no comprueba nada: lanza la consulta con `db_query()` y vuelve.
Cuando llegan las filas, **el Argon2 va a un `worker`** —
`ircd_pwhash_verify()` es puro justamente para esto— y el resultado se entrega
con `account_complete()` desde el hilo principal. Dos saltos, ninguna espera:
es la cadena completa que la fase 0 dejó montada, usada por primera vez de
punta a punta.

`ircd_pwhash_outdated()` decide si conviene rehacer el hash con los costes
actuales; se rehace en el mismo `worker`, después de contestar, para no sumar
latencia a la conexión.

---

## 8. Dónde se impone que el email no salga

No basta con no escribirlo. Se impone en cuatro sitios, y cada uno lleva su
prueba:

1. **La firma de `account_complete()`** no admite email. No hay por dónde.
2. **`struct Client` no tiene un campo para él.** Lo único que queda del
   *login* es `cli_user()->account`, que es el nick.
3. **`AccountRequest` se borra** con `ircd_crypto_wipe()` en cuanto el
   proveedor ha copiado lo suyo, secreto y email incluidos.
4. **Una prueba de integración** que autentica y luego revisa que el email no
   aparece en `WHOIS`, ni en `WHO`, ni en `STATS`, ni en nada que cruce el
   enlace entre dos servidores.

---

## 9. Orden de implementación

Cada punto compila, pasa pruebas y se puede subir por separado.

1. **`sasl.c` + `sasl.h`** — el registro de mecanismos, `PLAIN` y `EXTERNAL`,
   la capacidad `sasl` con su valor y su disponibilidad.  `sasl_t` lo prueba
   sin servidor.  *Nada visible todavía.*
2. **`account.c` + `account.h`** — el registro de proveedor, las peticiones en
   vuelo, `account_set()` / `account_clear()`.  `account_t`.
3. **`m_authenticate.c` + `AR_SASL_PENDING`** — el diálogo con el cliente y la
   retención del registro.  Con un proveedor de pruebas, ya se puede
   autenticar.
4. **`m_account.c`** — `ACCOUNT LOGIN` / `LOGOUT`, token `AC`, y la regla de
   §5 aplicada al nick.
5. **`modules/services/identity/`** — el almacén, las migraciones, el Argon2
   en un *worker*, el plazo de gracia de §6.
6. **Documentación y pruebas** — `doc/readme.accounting` reescrito entero,
   `doc/readme.sasl`, y las pruebas de integración, incluida la de §8.4.

---

## 10. Lo que esta fase no hace

- **No** mete la comprobación de credenciales en el core.
- **No** enruta SASL entre servidores: no hace falta (§2).
- **No** reintroduce `account-notify`, `account-tag` ni `extended-join`: con
  una cuenta que es el nick, son redundantes (§4.3).
- **No** añade un nombre de visualización libre. El nombre visible es el nick;
  lo bonito, si se quiere, va por `setname` y es una fase aparte.
- **No** veta nicks para protegerlos (§6).
- **No** hace 2FA ni SSO todavía: las columnas están para no migrar dos veces,
  pero los mecanismos que las usan (`OAUTHBEARER`, un `PLAIN` con segundo
  factor) son módulos posteriores, y el registro de §3.1 está para que quepan
  sin tocar el core.

---

## 11. Riesgos

- **Una base de datos caída deja a todo el mundo sin identificar.** Degrada
  bien —se entra sin `+r`—, pero el plazo de gracia de §6 debe fallar del lado
  seguro y no quitarle el nick a nadie cuando no puede comprobar nada.
- **Tres cuentas por email es poco para un bot.** Es lo decidido; si aparece el
  caso, se resuelve con un tipo de identidad distinto, no subiendo el número
  en silencio.
- **`ACCOUNT LOGIN` en claro.** Mitigado exigiendo TLS por omisión (§4.2), y la
  mitigación real es que exista SASL `EXTERNAL`.
- **El nick como identidad limita el producto**, y ya se dijo en la 006 §6.2.
  Esta propuesta lo asume, no lo resuelve.
