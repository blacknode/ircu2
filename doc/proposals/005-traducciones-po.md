# Propuesta 005 — Traducciones con ficheros PO, para el core y los módulos

**Estado:** aceptada, implementada (13 de septiembre de 2026; véase
`doc/readme.translations`)
**Depende de:** 001 (API de módulos; sube `IRCU_MODULE_ABI` porque `struct Client` cambia)
**Introduce:** `include/ircd_i18n.h`, `ircd/ircd_i18n.c`, `ircd/ircd_po.c`,
`ircd/m_language.c`, el directorio `po/` en la raíz y `po/` dentro de cada
módulo que quiera traducirse

## 1. Motivación

Todo lo que el servidor le dice a un usuario está en inglés y grabado en el
binario: los ~300 formatos de `replyTable[]` en `ircd/s_err.c`, los 138
`SND_EXPLICIT` repartidos por los `m_*.c`, los 117 `NOTICE` que el servidor
manda a un cliente en concreto, y ahora también lo que responden los bots de
`modules/services/`. Una red en español — o una red con usuarios de varios
países, que es el caso de cualquier red de cierto tamaño — no tiene manera de
cambiarlo sin tocar el código.

Lo que falta es que esto funcione:

```
[C] CAP LS 302
[S] :irc.example.net CAP * LS :draft/languages=3,es,en,pt ...
[C] LANGUAGE es-AR es
[S] :irc.example.net 687 * es-AR es :Se han fijado tus preferencias de idioma.
[C] NICK pepe
[C] USER pepe 0 * :Pepe
[S] :irc.example.net 001 pepe :Bienvenido a la red example, pepe
...
[C] WHOIS nadie
[S] :irc.example.net 401 pepe nadie :No existe ese nick
```

y que quien traduce trabaje con la herramienta que ya conoce — Poedit, Weblate,
Crowdin, `msgmerge` — sobre un formato que todas hablan, sin compilar nada y
sin tocar el servidor más que para un `/REHASH`.

## 2. Cómo se hace hoy: código legacy de ejemplo

La práctica en los forks de ircu en otros idiomas ha sido siempre la misma:
editar `s_err.c`.

```c
/* 401 */
  { ERR_NOSUCHNICK, "%s :No existe ese nick", "401" },
```

y, para los mensajes que no son numerics, cada sitio a mano:

```c
sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :El bot %s ha sido destruido", sptr, name);
```

Cuatro problemas, todos reales:

1. **Un idioma por binario.** El servidor entero habla español, también con el
   usuario que se conecta desde Lisboa. Elegir por usuario es imposible.
2. **Cada merge con upstream es un conflicto** en `s_err.c` y en cada `m_*.c`
   tocado, y la traducción se pierde con cada fichero que se reescribe.
3. **Los módulos quedan fuera.** Un módulo de terceros trae sus textos en su
   idioma, y para traducirlo hay que mantener un fork del módulo también.
4. **Quien traduce tiene que saber C** — y compilar, y no equivocarse con un
   `%s`: un `%d` cambiado por un `%s` en un formato es una lectura de memoria
   inválida en el servidor de producción.

## 3. Diseño

Un subsistema pequeño, propio, sin `libintl`: el servidor lee ficheros `.po`
directamente, los valida y los consulta al renderizar cada mensaje. La razón
de no usar gettext se explica en §9; en una frase: gettext elige el idioma por
*proceso* (o por hilo, con `uselocale()`), y aquí el idioma es del *cliente*,
y hay miles en el mismo hilo.

### 3.1 Catálogos: ficheros `.po` que lee el servidor

Un catálogo es un fichero `<código>.po` — `es.po`, `es-ar.po`, `pt.po` — en el
directorio de un *dominio*. El servidor lo lee con un parser propio
(`ircd/ircd_po.c`) que entiende el subconjunto de PO que produce cualquier
herramienta:

- `msgid`, `msgstr`, `msgctxt`, `msgid_plural`, `msgstr[N]`; cadenas entre
  comillas con los escapes de C (`\n`, `\t`, `\\`, `\"`, `\xhh`, `\ooo`) y
  continuación en líneas sucesivas.
- Comentarios `#`, `#.`, `#:`, `#|`. Los flags `#,`: una entrada `fuzzy` **se
  ignora** (es lo que hace `msgfmt`); las demás no importan. Las entradas
  obsoletas `#~` se ignoran.
- La cabecera (la entrada con `msgid ""`): `Content-Type` tiene que decir
  `charset=UTF-8` o no decir nada; otro charset rechaza el fichero.
  `Plural-Forms` se parsea (§3.9).
- Una entrada con `msgstr ""` está sin traducir y no entra en la tabla: se usa
  el texto original.

El código del idioma es el nombre del fichero: BCP 47 restringido a
`[A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*`, quince caracteres como máximo, comparado
sin distinguir mayúsculas y guardado en minúsculas. No hay `msgfmt` ni `.mo`:
el `.po` es lo que se instala y lo que se lee.

Un catálogo cargado es una tabla hash de `(msgctxt, msgid)` → `msgstr[]`. Un
fichero de mil entradas ocupa unos 100 KB y se parsea en menos de un
milisegundo; se carga en el hilo principal, como `ircd.conf`, sin worker.

### 3.2 Dominios: `core` y uno por módulo

Un dominio es un nombre y un directorio con catálogos. Hay uno para el ircd y
uno por módulo, sin búsquedas cruzadas: cada cual traduce lo suyo, igual que
`dgettext()`.

| Dominio | Directorio | Quién lo carga |
|---|---|---|
| `core` | `PO_PATH`, por defecto `$DPATH/po/` | `i18n_init()` al arrancar, `/REHASH` |
| `<módulo>` | `<module_dir()>/po/` | el loader de módulos al cargar, `/REHASH`, `/MODULE RELOAD` |

`PO_PATH` sigue el precedente de `MOD_PATH`: variable de caché `IRCU_POPATH`,
macro en `config.h`, expuesta en el resumen de configuración de CMake. En el
árbol de fuentes los catálogos del core viven en `po/` en la raíz (la
convención GNU) y se instalan en `PO_PATH`. Los de un módulo viven en
`modules/<tipo>/<nombre>/po/` y son *recursos* en el sentido de
`cmake/IrcuModules.cmake`: se copian junto al `.so` sin tocar el build. Un
módulo de un solo fichero (`<nombre>.c`) no tiene directorio y por tanto no
tiene traducciones — la misma regla que ya rige para las migraciones.

```c
struct I18nDomain;                          /* opaco */
extern struct I18nDomain* i18n_core;        /* el dominio del ircd */
```

El puntero a un dominio es estable mientras exista quien lo posee: un
`/REHASH` reemplaza los catálogos *detrás* del puntero, de forma atómica por
fichero, y un módulo que se guardó el suyo en `mi_init` sigue teniéndolo bueno.

### 3.3 Marcado en el código

Tres macros en `ircd_i18n.h`, con los nombres que `xgettext` espera:

```c
#ifndef I18N_DOMAIN
#define I18N_DOMAIN i18n_core
#endif
#define _(to, s)          i18n_text(I18N_DOMAIN, (to), (s))
#define _n(to, s, p, n)   i18n_ntext(I18N_DOMAIN, (to), (s), (p), (n))
#define N_(s)             (s)   /* marca para extraer; se traduce después */
```

`_()` y `_n()` traducen *ahora*, para el cliente `to`, y devuelven un
`const char*` que vive lo que viva el catálogo — se usa en la misma sentencia
y no se guarda. `N_()` no hace nada: marca un literal que se traducirá más
tarde, en el sitio que sí conoce al destinatario.

Ese sitio es `send_reply()`. Es la única función que sabe a la vez el numeric,
el formato y el cliente, y hoy ya elige entre `num->format` y el formato
explícito de `SND_EXPLICIT`; pasa a traducir el que elija:

```c
if (reply & SND_EXPLICIT)
  vd.vd_format = i18n_text(i18n_core, to, va_arg(vd.vd_args, char *));
else
  vd.vd_format = i18n_ctext(i18n_core, to, num->str, num->format);
```

Los numerics se buscan con **contexto = el código del numeric** (`"401"`), que
ya es un campo de `struct Numeric`: dos numerics con el mismo texto en inglés
pueden traducirse distinto y quien traduce ve de qué numeric se trata. La
tabla de `s_err.c` se escribe con una macro local que `xgettext` extrae con
`--keyword=N:2c,3`:

```c
#define N(sym, str, fmt) { sym, fmt, str }
/* 401 */
  N(ERR_NOSUCHNICK, "401", "%s :No such nick"),
```

Los formatos explícitos se marcan `N_()` donde están:

```c
send_reply(sptr, SND_EXPLICIT | RPL_REHASHING, N_(":Flushing MOTD cache"));
```

y los `NOTICE` a un cliente, con `_()`, traduciendo el formato entero — el
`%C` y el resto de directivas viajan dentro y la validación de §3.8 garantiza
que siguen ahí:

```c
sendcmdto_one(&me, CMD_NOTICE, sptr, _(sptr, "%C :Bot %s destroyed"), sptr, name);
```

Nada de esto cambia el coste para un cliente sin idioma: `i18n_text()` con un
cliente sin preferencia y sin `DEFAULT_LANGUAGE` es una comparación y un
`return`.

### 3.4 Qué se traduce y qué no

**Se traduce lo que se genera para un único cliente**: `send_reply()`, un
`NOTICE`/`PRIVMSG` del servidor o de un bot a un cliente (`sendcmdto_one(&me,
CMD_NOTICE, …)`, `bot_send_user()`), y los avisos previos al registro (`NOTICE
AUTH`), que con `DEFAULT_LANGUAGE` puesto salen ya en el idioma del servidor.

**No se traduce nunca**:

- el log (`log_write()`): lo leen herramientas, y un log en varios idiomas no
  lo lee nadie;
- lo que va a varios destinatarios a la vez — snotices de opers, `WALLOPS`,
  mensajes a un canal — porque se renderiza una vez para todos;
- lo que cruza el enlace P10 como texto — motivos de `KILL`, `QUIT` y `ERROR`,
  `SQUIT` — porque lo verán clientes de cualquier idioma y otros servidores;
- `ERROR` a un servidor, `ircd -v`, `ircd -k`, el `Usage`.

La regla cabe en una frase y evita el error clásico de traducir un `KILL`
reason que luego aparece en el `QUIT` de todos los canales.

### 3.5 El idioma de cada cliente

Un cliente tiene una *preferencia*: hasta `I18N_PREF_MAX` (3) códigos en
orden. Se guarda en `struct Client` — no en `Connection`, porque hace falta
antes del registro (§3.6) y para clientes remotos (§3.7) — como un índice de
16 bits en una tabla de preferencias distintas internadas: cien mil clientes
con tres preferencias distintas cuestan doscientos KB, no cinco MB, y no hay
`MyMalloc()`/`MyFree()` por cliente. La tabla está acotada
(`I18N_PREF_TABLE_MAX`, 4096) igual que la de códigos vistos (256); pasado el
tope, una preferencia nueva se reduce a su primer código y se deja una línea
en `LS_SYSTEM` una sola vez.

La cadena de búsqueda para un mensaje es, en orden:

1. cada código de la preferencia del cliente, y para cada uno el catálogo
   exacto y luego el de su subetiqueta primaria (`es-ar` → `es-ar`, `es`);
2. `FEAT_DEFAULT_LANGUAGE`, con la misma expansión;
3. el texto original.

La cadena se resuelve a punteros a catálogo cuando se interna la preferencia y
se vuelve a resolver en cada `/REHASH`; por mensaje son como mucho ocho
sondeos en una hash, y sólo para clientes con preferencia.

`FEAT_DEFAULT_LANGUAGE` es una feature de cadena, vacía por defecto (= texto
original), que se cambia con `/SET` como cualquier otra. Es lo que hace que
`irc.es.example.net` hable español y `irc.us.example.net` inglés con la misma
imagen y el mismo `po/`.

### 3.6 Negociación: `draft/languages` y `LANGUAGE`

No hay un estándar ratificado. Existe un borrador IRCv3 (PR #150 de
ircv3-specifications, "IRCv3.3 Language Negotiation", cerrado sin fusionar) y
una implementación, la de Ergo, que lo publica como `draft/languages`. Se
sigue ese borrador tal cual, con ese nombre, para que un cliente que ya lo
hable funcione:

- **Capability** `draft/languages`, con valor (sólo para `CAP LS 302`, que
  `m_cap.c` ya distingue): `<máximo>,<código>,...` — el máximo de códigos que
  admite `LANGUAGE`, y luego los idiomas disponibles, primero el del servidor
  (`DEFAULT_LANGUAGE`), después el resto en orden alfabético. `en` — el
  idioma de los textos originales — está siempre, no necesita catálogo. El
  valor se recalcula tras cada `/REHASH` con `cap_set_value()`. Feature
  `FEAT_CAP_LANGUAGES`, activa por defecto, como las demás `FEAT_CAP_*`.
- **Comando** `LANGUAGE <código> [<código>...]`, para clientes registrados y
  sin registrar — la negociación ocurre antes de `NICK`/`USER`, y es lo que
  permite que el 001 ya salga traducido. El `CAP REQ` no es obligatorio: la
  cap sólo anuncia.
- **Numerics**, en los huecos que el borrador reserva y que están libres en
  `numeric.h` (`ERR_LASTERROR` pasa de 909 a 983):

  | Numeric | Nº | Forma |
  |---|---|---|
  | `RPL_YOURLANGUAGESARE` | 687 | `<código>... :Language preferences have been set.` |
  | `RPL_WHOISLANGUAGE` | 690 | `<nick> <código>... :can speak these languages.` |
  | `ERR_TOOMANYLANGUAGES` | 981 | `<máximo> :You specified too many languages.` |
  | `ERR_NOLANGUAGE` | 982 | `<código>... :Languages are not supported by this server.` |

  El 687 sale ya en el idioma recién elegido. El 982 lista sólo los códigos
  que no se admiten, y la preferencia no cambia. Sin parámetros, 461.
- **WHOIS** muestra el 690 al propio usuario y a los opers, como se hace ya
  con el 338: la preferencia de idioma dice de dónde es alguien.

### 3.7 Propagación en P10

Un mensaje se traduce donde se *genera*, y no siempre es el servidor del
usuario: un `/WHOIS nick servidor`, un `/STATS` remoto, y sobre todo la
respuesta de un bot de `irc_services` a alguien conectado a otro servidor.
Para que eso salga en el idioma correcto, el servidor que genera tiene que
conocer la preferencia; el borrador recomienda propagarla, y este proyecto ya
controla el P10 de toda la red (`doc/readme.accounting`).

Un token nuevo, `LG` (`MSG_LANGUAGE`/`TOK_LANGUAGE`; `LG` está libre), con la
misma forma que el comando de cliente:

```
ABAAB LG es-ar es
```

Sale en tres momentos, siempre con el usuario como origen:

1. cuando un usuario registrado cambia su preferencia con `LANGUAGE`;
2. justo después de su `N` en `register_user()`, si la fijó antes de
   registrarse;
3. en el burst, después de la `N` de cada usuario que tenga preferencia — el
   sitio exacto donde `s_serv.c` ya manda el `AWAY` de `FEAT_AWAY_BURST`. No
   lleva feature: son pocas líneas y sin ellas el resto no sirve.

Al recibirlo (`ms_language`), el servidor guarda la preferencia tal cual llega,
sin validarla contra sus propios catálogos: un hub que no tiene `fr-ca` tiene
que poder reenviarla entera al siguiente servidor. Los códigos se recortan a
`I18N_LANG_MAX` y a `I18N_PREF_MAX`, y lo que no cumpla la sintaxis de §3.1 se
descarta con un `protocol_violation()`.

Un servidor que reciba `LG` sin conocerlo lo trata como cualquier comando
desconocido de un servidor; como con `+r`, la red tiene que ejecutar el mismo
código. No hay nada nuevo en la `N`: la línea de burst no cambia de formato.

### 3.8 Validación al cargar: la propiedad de seguridad

Un `.po` está en `DPATH`, es del administrador y merece la confianza de
`ircd.conf` — pero no la de quien lo escribió, que es un traductor, y una
traducción es un **formato** de `ircd_snprintf()`. Un `%s` de más o un `%d`
donde había un `%s` es leer memoria que no toca en el servidor de producción.
El loader no acepta ninguna entrada sin comprobar, en este orden:

1. `msgstr` no contiene `\r`, `\n` ni `\0`: un mensaje IRC es una línea, y un
   `\n` en una traducción sería un segundo comando inyectado.
2. Las **directivas** de `msgstr` son las mismas que las de `msgid`, en el
   mismo orden, byte a byte — flags, anchura, precisión, modificador y
   conversión, con la gramática de `ircd_snprintf` (`%:#C`, `%Tu`, `%v`, `%*s`
   incluidos). `%%` puede aparecer donde se quiera. Sólo el texto entre
   directivas es libre.
3. En una entrada con plural, cada `msgstr[N]` cumple 1 y 2 contra
   `msgid_plural`, y hay exactamente `nplurals` formas.
4. El fichero es UTF-8 válido.

Una entrada que falla **se descarta y se usa el original**, con una línea en
`LS_CONFIG` que nombra fichero, línea y motivo; el resto del fichero se carga.
Un fichero que no parsea (cadena sin cerrar, cabecera ilegal) se rechaza entero
y se **conserva la versión anterior** si la había, que es lo que hace un
`/REHASH` con un bloque roto. El oper que rehashea recibe el mismo texto que el
log, como con los errores de configuración.

La regla 2 es más estricta de lo que gettext exige: `ircd_snprintf` no soporta
`%n$`, de modo que una traducción no puede reordenar argumentos. En la
práctica casi ningún mensaje tiene más de dos, y se reescribe la frase
alrededor; añadir `%n$` está en §10.

`ircd -k` carga los catálogos además de la configuración y sale con error si
alguno se rechaza, para que un despliegue lo compruebe antes de arrancar.

Lo que no comprueba el loader: la longitud. Una traducción suele ser más larga
que el original y un numeric ya se trunca a 512 bytes en `msgq_make()`; es el
comportamiento de siempre y vale igual aquí.

### 3.9 Plurales

`_n(to, "%d user", "%d users", n)` con la `Plural-Forms` de la cabecera
(`nplurals=2; plural=(n != 1);`). El evaluador es un descenso recursivo sobre
el subconjunto de C que usan todas las reglas conocidas — `? :`, `||`, `&&`,
`== != < > <= >=`, `+ - * / %`, paréntesis, enteros y `n` — con `nplurals`
acotado a 8 (el árabe usa 6). Una cabecera sin `Plural-Forms` deja el catálogo
sin plurales: `_n()` devuelve el original.

El core no usa plurales hoy (`"%d users"` y compañía vienen de los años
noventa); la macro existe desde el principio para que un texto nuevo, del core
o de un módulo, no tenga que elegir entre "1 usuarios" y una tabla propia.

## 4. Ejemplo completo

Árbol de fuentes:

```
po/core.pot                          plantilla generada (target `pot`)
po/es.po
modules/services/irc_services/po/es.po
modules/commands/m_bot/po/es.po      (m_bot.c pasa a directorio para tenerlo)
```

Instalado:

```
$DPATH/po/es.po
$DPATH/modules/services/irc_services/irc_services.so
$DPATH/modules/services/irc_services/po/es.po
```

`po/es.po`, un fragmento:

```po
msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\n"
"Plural-Forms: nplurals=2; plural=(n != 1);\n"

#: ircd/s_err.c:37
msgctxt "001"
msgid ":Welcome to the %s IRC Network%s%s, %s"
msgstr ":Bienvenido a la red %s%s%s, %s"

#: ircd/s_err.c:837
msgctxt "401"
msgid "%s :No such nick"
msgstr "%s :No existe ese nick"

#: ircd/m_rehash.c:113
msgid ":Flushing MOTD cache"
msgstr ":Vaciando la caché del MOTD"

#: ircd/s_err.c:997
#, fuzzy
msgctxt "481"
msgid ":Permission Denied: Insufficient privileges"
msgstr ":Permiso denegado"
```

La última entrada es `fuzzy` y no se carga: el 481 sigue en inglés hasta que
alguien la revise, que es exactamente lo que `fuzzy` significa.

`ircd.conf`:

```
Features {
        "DEFAULT_LANGUAGE" = "es";
};
```

Y la sesión del §1 tal cual: el cliente que no negocia nada recibe español por
la feature; el que pide `LANGUAGE en` recibe el original; el que pide
`LANGUAGE pt` recibe 982 si no hay `pt.po`.

## 5. Ciclo de vida

- **Arranque.** `i18n_init()` corre después de leer la configuración (necesita
  `PO_PATH` y la feature) y antes de abrir los listeners: carga `core` y fija
  el valor de la cap. Un catálogo roto no impide arrancar — se avisa y ese
  idioma queda incompleto o ausente — salvo con `-k`, donde es un error.
- **`/REHASH`.** Recarga todos los dominios, `core` y módulos, con las reglas
  de §3.8, recalcula la cap y vuelve a resolver las cadenas de §3.5. Un
  `/REHASH m`, `l` o `s` no toca los catálogos.
- **`/MODULE LOAD`.** Si `<module_dir()>/po/` existe, el loader abre el
  dominio antes de `mi_init`, así que `module_i18n(mod)` ya vale ahí. Un
  catálogo roto no impide cargar el módulo: aplica la misma regla que al core.
  `/MODULE UNLOAD` cierra el dominio después de `mi_fini`; `RELOAD` es las dos
  cosas.
- **Salida de un cliente.** Nada que liberar: la preferencia es un índice.
- **Cambio de nick, KILL, colisión.** No afectan; la preferencia va con el
  `struct Client`.

`/STATS languages` (letra `n`, libre) lista cada dominio con sus idiomas, el
número de entradas cargadas y de rechazadas en la última carga, para saber si
un `/REHASH` dejó algo fuera sin bucear en el log.

## 6. Módulos

Un accesor nuevo en `module.h`:

```c
/** El dominio de traducciones del módulo, cargado desde <dir>/po/ al
 * cargarlo, o NULL si no trae ninguno.  i18n_text() con NULL devuelve el
 * original, así que un módulo sin traducciones no necesita comprobarlo. */
extern struct I18nDomain* module_i18n(const struct ModuleHandle* mod);
```

y el patrón de uso, entero:

```c
#define I18N_DOMAIN mod_i18n
#include "ircd_i18n.h"
#include "module.h"
#include "bot.h"

static struct I18nDomain* mod_i18n;

static int init(struct ModuleHandle* mod)
{
  mod_i18n = module_i18n(mod);
  ...
}

static void reply(struct Client* bot, struct Client* to, unsigned int n)
{
  char buf[BUFSIZE];
  ircd_snprintf(NULL, buf, sizeof(buf),
                _n(to, "You have %u pending request",
                       "You have %u pending requests", n), n);
  bot_send_user(bot, to, 1, buf);
}
```

El módulo traduce con su dominio *antes* de entregar el texto; `bot_send_user()`
y `send_reply()` no saben de qué módulo viene. Cuando un módulo pasa un
`SND_EXPLICIT` ya traducido, `send_reply()` lo busca en `core`, no lo
encuentra y lo deja tal cual — un sondeo de más, y ninguna ambigüedad sobre
quién es dueño de cada cadena.

Todo lo demás sigue las reglas de 001: el dominio se abre y se cierra con el
módulo, un módulo no ve el dominio de otro, y nada de esto corre en un worker
— `i18n_text()` toca `struct Client` y no puede llamarse desde un hilo, como
cualquier otra cosa que lo toque.

## 7. Implementación

| Fichero | Qué |
|---|---|
| `include/ircd_i18n.h` | API, macros, límites, reglas de validación, con Doxygen. |
| `ircd/ircd_po.c` | Parser de PO y evaluador de `Plural-Forms`. Sin dependencias del core más allá de `ircd_alloc.h` e `ircd_log.h`, para poder testearlo solo. ~600 líneas. |
| `ircd/ircd_i18n.c` | Dominios, catálogos, tabla de preferencias, cadenas de búsqueda, validación de directivas, valor de la cap, `/STATS`. ~700 líneas. |
| `ircd/m_language.c` | `m_language` (cliente, registrado o no), `ms_language` (`LG`). |
| `ircd/s_err.c` | La tabla con la macro `N()`; cuatro numerics nuevos. |
| `ircd/ircd_reply.c` | `send_reply()` traduce el formato elegido. |
| `ircd/s_user.c`, `ircd/s_serv.c` | `LG` tras la `N` en el registro y en el burst. |
| `ircd/m_whois.c`, `ircd/m_cap.c` | 690; `draft/languages` en `CAPLIST`. |
| `ircd/module.c` | Abre y cierra el dominio del módulo; `module_i18n()`. |
| `include/client.h` | `cli_lang()`; `IRCU_MODULE_ABI` +1. |
| `include/msg.h`, `ircd/parse.c` | `MSG_LANGUAGE`, `TOK_LANGUAGE`, la entrada de `msgtab[]`. |
| `ircd/ircd_features.c` | `DEFAULT_LANGUAGE`, `CAP_LANGUAGES`. |
| `cmake/IrcuPaths.cmake`, `cmake/config.h.cmake.in` | `IRCU_POPATH` → `PO_PATH`. |
| `po/`, `CMakeLists.txt` | Instalación de `po/*.po` en `PO_PATH`; target `pot` (§8). |
| `doc/readme.translations` | Para quien traduce y quien administra. |

El marcado del código se hace por capas, cada una útil sola: primero
`s_err.c` (una macro y una tabla; con eso ya está el 90 % de lo que ve un
usuario), luego los `SND_EXPLICIT`, luego los `NOTICE` a un cliente, luego
`m_bot` e `irc_services`. Ninguna capa bloquea a la siguiente ni rompe nada si
se queda a medias: lo no marcado sale en inglés.

Límites, todos en el header: `I18N_LANG_MAX` 16, `I18N_PREF_MAX` 3,
`I18N_PREF_TABLE_MAX` 4096, `I18N_NPLURALS_MAX` 8, `I18N_PO_LINE_MAX` 4096.

## 8. Pruebas

`ctest -R ircd_i18n_t`, sin servidor, sobre `ircd_po.c` e `ircd_i18n.c`:

- parser: escapes, continuación, `msgctxt`, plurales, `fuzzy`, `#~`, cabecera
  con y sin `Plural-Forms`, charset que no es UTF-8, fichero truncado;
- validación: cada regla de §3.8 con un caso que pasa y uno que no, incluidas
  las directivas exóticas (`%:#C`, `%Tu`, `%v`, `%*.*s`, `%%`);
- búsqueda: la cadena de §3.5 completa, `es-ar` → `es` → default → original,
  y la resolución tras un reload que quita un catálogo;
- plurales: las reglas de `en`, `fr`, `ru`, `pl`, `ar` contra tablas de
  valores conocidos;
- catálogos del árbol: el test carga **todos** los `.po` de `po/` y de
  `modules/*/*/po/` y falla si alguna entrada se rechaza. Es lo que sustituye
  a `msgfmt --check`, que no entiende `%C` ni `%Tu`, y es lo que corre en CI.

Integración, en `tests/`, con la topología de un solo servidor (la imagen se
construye desde el árbol y `es.po` se instala con el resto): `CAP LS 302`
muestra `draft/languages`; `LANGUAGE es` antes de registrar da un 001 en
español; `LANGUAGE xx` da 982; cuatro códigos dan 981; `WHOIS` de un nick
inexistente da un 401 en español y, tras `LANGUAGE en`, en inglés. Con la
topología completa: un usuario con `LANGUAGE es` en la hoja recibe en español
la respuesta de un bot de `irc_services` que vive en el hub.

Herramientas de desarrollo, opcionales y nunca dependencias del build: el
target `pot` corre `xgettext` si está instalado —

```
xgettext --language=C --from-code=UTF-8 --add-comments=TRANSLATORS \
  --keyword=_:2 --keyword=_n:2,3 --keyword=N_ --keyword=N:2c,3 \
  --flag=_:2:no-c-format --flag=N_:1:no-c-format \
  -o po/core.pot ircd/*.c
```

— y `msgmerge -U po/es.po po/core.pot` actualiza una traducción. `no-c-format`
evita que las herramientas apliquen la comprobación de C a formatos que no lo
son; la comprobación buena es la del loader.

## 9. Alternativas descartadas

- **`libintl` / gettext.** El idioma es del proceso (`setlocale()`) o del
  hilo (`uselocale()`), y cambiarlo por mensaje en un hilo que sirve a miles de
  clientes es lento, no reentrante y ajeno a lo que un `.mo` pretende. Además:
  paso de compilación (`msgfmt`), formato binario que nadie edita, dependencia
  con tres implementaciones distintas (glibc, musl con stub, libintl aparte en
  BSD), y ninguna validación de las directivas propias de `ircd_snprintf`. Lo
  único que se pierde es la caché de plurales y el `LANGUAGE` de entorno, que
  aquí no tienen sentido.
- **Compilar los `.po` dentro del binario, como las migraciones.** Las
  migraciones son código y deben ir con el `.so` que las usa; una traducción
  es un dato que un administrador añade después de instalar — un `pt.po` en
  `$DPATH/po/` sin recompilar — y que un traductor corrige con un `/REHASH`.
- **JSON o YAML propios**, como hace Ergo. Ni Poedit, ni Weblate, ni Crowdin,
  ni `msgmerge`, ni memoria de traducción: se pierde todo el ecosistema para
  ahorrar un parser de trescientas líneas.
- **Un idioma por servidor, sin negociación.** Es `FEAT_DEFAULT_LANGUAGE` a
  secas, y es el primer paso, pero deja al usuario de Lisboa sin opción y no
  resuelve las respuestas remotas.
- **La preferencia como modo de usuario con parámetro.** P10 no tiene modos de
  usuario con parámetro (`+r` acaba de dejar de tenerlo), y un modo se ve en
  `MODE` y se propaga en la `N`; un token propio es más simple y más
  discreto.
- **Traducir en `ircd_snprintf()`**, con un idioma "actual" global. Es el
  error de gettext otra vez, y además esconde el destinatario.
- **`%n$` en `ircd_snprintf` desde el principio.** Toca la ruta caliente de
  todo el servidor para una necesidad que la validación de §3.8 convierte en
  "reescribe la frase". Se pospone (§10) hasta que un idioma real lo necesite.

## 10. Trabajo futuro

- `%n$` en `ircd_snprintf()` y, con ello, relajar la regla 2 de §3.8 a "las
  mismas directivas, en cualquier orden".
- `Client { language = "es"; };` como valor por defecto por bloque, entre la
  preferencia y la feature, para redes con varios países en un mismo servidor.
- MOTD por idioma: `Motd { language = "es"; file = "es.motd"; };` en el
  mismo mecanismo de `motd.c` que ya elige MOTD por host y por clase.
- Que un bot `+S` pueda fijar la preferencia de un usuario
  (`bot_set_language()`, que sale como `LG`), para servicios que la guardan
  en su base de datos y la restauran al identificarse.
- `HELP` traducido: el fichero de ayuda es texto plano y cabe en el mismo
  esquema que el MOTD.
- Extraer los numerics con un `#.` que diga `ERR_NOSUCHNICK` además del
  contexto `"401"`: es un postproceso trivial del `.pot`, y a los traductores
  les ayuda.
