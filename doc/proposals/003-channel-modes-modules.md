# Propuesta 003 — Modos de canal registrables por módulo

**Estado:** aceptada, implementada
**Depende de:** 001 (API de módulos), ABI 2
**Introduce:** ABI 3

## 1. Motivación

Un módulo ya podía añadir un modo de usuario (`module_add_user_mode()`, ABI 2).
La mitad simétrica faltaba: casi todo lo que un operador de red quiere añadir sin
tocar el core es una política **por canal** — "aquí no se grita", "aquí sólo
hablan cuentas verificadas", "aquí no se cambia de nick". Eso obligaba a parchear
`ircd/channel.c`, y cada parche local se paga en cada actualización.

El objetivo es que esto funcione, con la misma forma que tiene el registro de
modos de usuario:

```c
static chanmode_t nocaps_flag;

if (!module_add_chan_mode(mod, 'G', &nocaps_flag))
  return -1;
```

## 2. Por qué no bastaba con copiar el registro de umodes

El registro de modos de usuario se pudo hacer porque antes hubo un refactor
(`8c102e4`): `enum Flag` → máscara `flag_t` de 64 bits, y tabla estática → lista
enlazada. Los modos de canal estaban un paso más atrás y además tenían tres
problemas propios.

### 2.1 El espacio de bits estaba lleno y contaminado

`struct Mode.mode` era `unsigned int`, y en esa misma palabra convivían las letras
y banderas internas que no son modos:

| bits | uso |
|---|---|
| 0–15 | `o v p s m t i n k b l r D R c C` |
| 16 | libre |
| 17–19 | `MODE_SAVE`, `MODE_FREE`, `MODE_BURSTADDED` — internos, no son letras |
| 20–26 | `U A d u M Z z` |
| 27, 28 | libres |
| 29, 30 | `MODE_DEL`, `MODE_ADD` — dirección del cambio, no son letras |
| 31 | libre |

Cuatro bits libres en total, y los bits de control mezclados con las letras.

### 2.2 La correspondencia letra↔bit estaba copiada cinco veces

`static int flags[]` / `chan_flags[]` aparecía en `modebuf_flush_int()` (con
`local_flags[]` y `global_flags[]` además), `modebuf_extract()`, `mode_parse()`,
`do_clearmode()` y como máscara literal en `modebuf_mode()`. `channel_modes()`
escribía las letras a mano, `if` a `if`. Un modo nuevo había que darlo de alta en
seis sitios; uno dinámico no se podía dar de alta en ninguno. Y eran arrays de
`int`: no admiten un flag de 64 bits ni aunque se ensanche el tipo.

### 2.3 Se anunciaban constantes, no el estado real

`infochanmodes`, `infochanmodeswithparams` y el `CHANMODES=` de `RPL_ISUPPORT`
eran cadenas literales. Igual que pasó con `infousermodes` en ABI 2, tienen que
construirse desde el registro o el cliente no se entera de que el modo existe.

### 2.4 `HOOK_CHANNEL_PRE_MODE` estaba declarado pero no se despachaba

`include/hooks.h` lo declaraba; no había ningún `hook_run(HOOK_CHANNEL_PRE_MODE,
...)` en el árbol. Sin él, un módulo puede registrar la letra pero no tiene forma
de decir quién puede ponerla — el equivalente exacto de lo que
`HOOK_CLIENT_PRE_UMODE` hace para los umodes.

## 3. Alcance

**Dentro:** modos de canal booleanos, sin parámetro, aplicados al canal,
propagados por la red. Es exactamente lo que hoy es un modo de módulo de usuario.

**Fuera (por ahora):** modos con parámetro (tipo A/B/C de `CHANMODES`) y modos de
miembro (`+o`/`+v`). Un modo con argumento toca las seis ranuras de
`mb_modeargs[]`, el orden obligatorio del BURST, la reserva de `MAXMODEPARAMS` y
el ciclo de vida de la cadena (`MODE_FREE`). Es una propuesta aparte; el registro
que se define aquí ya lleva el campo de atributos para que quepa sin romper la
ABI de datos, pero la API pública de módulo no lo expone.

## 4. Diseño

### 4.1 El bit sale de la letra

`include/chan_flags.h`, hermano de `include/user_flags.h`:

```c
typedef unsigned long long chanmode_t;
```

El bit de un modo es función de su letra, la misma convención que ya seguían los
`FLAG_*` de los modos de usuario:

    'A'..'Z'  ->  bits 0..25
    'a'..'z'  ->  bits 26..51

```c
#define ChanModeBit(c) ((c) >= 'a' ? ((c) - 'a' + 26) : ((c) - 'A'))
```

Es la decisión que sostiene todo lo demás. **Nadie reparte bits y nadie los
elige**, así que:

- dos servidores construidos de las mismas fuentes coinciden en cada bit sin
  tener que decirse nada;
- el modo de un módulo cae en el mismo bit en todos los servidores que lo
  carguen, sin depender del orden de carga;
- no hay asignador que pueda agotarse ni bit que "se libere": la letra es el bit,
  siempre.

Existen 52 letras, así que los doce bits que quedan por encima (52–63) no puede
reclamarlos ningún modo. Ahí viven los que no son modos:

```c
#define MODE_BURSTADDED (BITSET << 59)
#define MODE_FREE       (BITSET << 60)
#define MODE_SAVE       (BITSET << 61)
#define MODE_DEL        (BITSET << 62)
#define MODE_ADD        (BITSET << 63)

#define CHANMODE_RESERVED (~CHANMODE_LETTER_MASK)
```

`channel_append_chan_mode()` rechaza cualquier flag que toque `CHANMODE_RESERVED`
o que no sea el que la letra implica.

### 4.2 El registro

`ircd/chan_modes.c` — una unidad de traducción aparte, porque es una cosa pequeña
y cerrada con su propia prueba, y porque el cargador de módulos la necesita sin
necesitar el resto del código de canales.

```c
struct ChanMode {
  chanmode_t       flag;  /* sale de c */
  char             c;     /* la letra */
  char             alt;   /* letra hacia servidores cuando difiere (z -> Z) */
  unsigned int     attr;  /* CHANMODE_* */
  unsigned int     count; /* nº de modos registrados; sólo en la cabeza */
  struct ChanMode *next;
};

extern void channel_init_chan_modes(void);
extern const struct ChanMode *channel_chan_modes(void);
extern const struct ChanMode *channel_find_chan_mode(char c);
extern chanmode_t channel_chan_mode_flag(char c);
extern int channel_check_chan_mode(char c, chanmode_t flag);
extern int channel_append_chan_mode(char c, chanmode_t flag);
extern int channel_remove_chan_mode(char c);
extern const char *channel_chan_mode_chars(void);
extern const char *channel_chan_mode_param_chars(void);
extern const char *channel_chanmodes_supported(void);
```

**La lista se mantiene ordenada por bit**, que es el orden de las letras. Un
servidor renderiza siempre la misma cadena de modos para el mismo canal,
independientemente del orden en que se hayan registrado los módulos. Los módulos
la ven a través de `channel_chan_modes()`, **como puntero a const**: el registro
es del servidor, y las únicas puertas son `module_add_chan_mode()` y
`module_del_chan_mode()`.

El campo `attr` absorbe las excepciones que antes estaban dispersas:

```c
#define CHANMODE_PARAM      0x01 /* lleva argumento: k l b A U */
#define CHANMODE_PARAM_SET  0x02 /* ...sólo al ponerlo (+l) */
#define CHANMODE_LIST       0x04 /* modo de lista (+b) */
#define CHANMODE_MEMBER     0x08 /* se aplica a un miembro: o v */
#define CHANMODE_LOCAL      0x10 /* sólo visible para clientes locales: d z */
#define CHANMODE_SERVERONLY 0x20 /* sólo un servidor, o MODE_PARSE_FORCE: R */
#define CHANMODE_INTERNAL   0x40 /* lo gestiona el core; MODE no lo acepta: d z */
#define CHANMODE_OPLEVELS   0x80 /* sólo con FEAT_OPLEVELS: A U */
#define CHANMODE_HIDDEN    0x100 /* no se anuncia en ISUPPORT CHANMODES: z */
```

Con eso desaparecen el `if (flag_p[0] == MODE_REGISTERED && ...)` de
`mode_parse_mode()`, la máscara literal de `modebuf_mode()` y los tres arrays de
`modebuf_flush_int()`. **Un modo de módulo no lleva ningún atributo**: es un flag
liso sobre el canal, global, que ponen los ops.

Se quedan fuera del registro, como semántica del core, las tres reglas que no son
"una letra es un bit": la exclusión mutua `+s`/`+p`, el par `+D`/`+d` y el par
`Z`/`z` (el único modo cuya letra depende de quién lo lee, que es para lo que
está `alt`).

**Macros de acceso**, hermanas de `HasUFlag`/`SetUFlag`/`ClrUFlag`:

```c
#define HasCFlag(chptr, flag) (((chptr)->mode.mode & (flag)) != 0)
#define SetCFlag(chptr, flag) ((chptr)->mode.mode |= (flag))
#define ClrCFlag(chptr, flag) ((chptr)->mode.mode &= ~(flag))
```

### 4.3 La API de módulo (ABI 3)

```c
extern int module_add_chan_mode(struct ModuleHandle *mod, char mode,
                                chanmode_t *flag);
extern int module_del_chan_mode(struct ModuleHandle *mod, char mode);
extern unsigned int module_chan_mode_count(const struct ModuleHandle *mod);
```

**Reglas de colisión.** Falla si la letra no es `A-Z`/`a-z`, si ya la tiene el
core o si otro módulo llegó antes. `module_del_chan_mode()` sólo recorre la lista
del propio módulo: no se puede quitar un modo del core ni el de otro módulo.

**Reversión.** Al descargar, `channel_remove_chan_mode()` recorre
`GlobalChannelList` quitando el modo de todos los canales que lo tuvieran y
anunciando un `-<letra>` normal **a los miembros y a la red**
(`MODEBUF_DEST_CHANNEL | MODEBUF_DEST_SERVER | MODEBUF_DEST_OPMODE`). El anuncio
va antes de desenlazar el nodo: `modebuf_flush()` renderiza la letra recorriendo
ese mismo registro y no emitiría nada una vez desenlazado — el mismo cuidado que
en los umodes.

Propagar es deliberado, y es la única asimetría real con los modos de usuario: un
usuario pertenece a un servidor, un canal es de la red. Si el modo se quedara
puesto en los demás nodos mientras éste ya no lo implementa, el canal creería
tener una política que se aplica a medias.

**Convención de despliegue.** Dos servidores de una red corren la misma versión
de las fuentes, y por la misma convención cargan los mismos módulos. Un servidor
sin el módulo no reconoce la letra y responde `ERR_UNKNOWNMODE` a un cliente
local; en un BURST la descarta en silencio. Mantener la lista de módulos
sincronizada es hoy responsabilidad del operador de la red.

Eso se arreglará: más adelante la API se extenderá para **propagar la carga y la
descarga de módulos**, lo que obliga a ajustar P10, a publicar la lista de
módulos en el BURST y a subir el protocolo a **P11**, que pasaría a ser la única
versión soportada, sin retrocompatibilidad. Esta propuesta no lo aborda; lo
menciona porque es la razón por la que la sincronización se deja en manos del
operador en lugar de inventar aquí un mecanismo a medias.

**El formato de red no cambia.** Por el cable viajan letras, no bits: ensanchar
`chanmode_t` es un cambio puramente interno y no toca P10.

### 4.4 Darle sentido a la letra

Registrar la letra es la mitad de un modo. La otra mitad es un hook.

`HOOK_CHANNEL_PRE_MODE` **se despacha** ahora, en `mode_parse()`, antes de aplicar
nada y sólo para clientes locales (los cambios que llegan de otro servidor ya
fueron aceptados por la red; rechazarlos aquí desincronizaría el canal). Es el
espejo del bloque de `set_user_mode()`.

En `mode_parse()`, la letra se resuelve contra el registro, de modo que un modo de
módulo se busca exactamente igual que uno del core. Los permisos siguen siendo los
del core: hace falta ser op. Un módulo que quiera restringir más el suyo usa el
hook.

**Módulo de referencia: `modules/cmode_nocaps.c`, modo `+G`.** Es
`modules/nocaps.c` condicionado al canal: mismo hook
(`HOOK_MESSAGE_PRE_CHANNEL`, que ya recibe `hc_channel`), con un `HasCFlag()` de
más. La diferencia entre los dos módulos es justo lo que aporta un modo: la misma
regla, pero pedida en lugar de impuesta en todos los canales.

## 5. Consecuencias del cambio de anchura

- `MODE_CHANOP` y `MODE_VOICE` ya no son `CHFL_CHANOP` y `CHFL_VOICE`.
  `chan_member_status()` convierte entre el flag del modo y el estado del
  miembro; son tres sitios en `channel.c`.
- `MODE_ADD`/`MODE_DEL` son bits de `chanmode_t` y no caben en un `int`, así que
  los modos de usuario usan sus propios `UMODE_ADD`/`UMODE_DEL`: la dirección de
  un cambio de modo de usuario nunca fue una bandera de canal.
- `netride_modes()` devolvía la máscara en un `int` y usaba -1 como error; ahora
  deja la máscara en un parámetro de salida y reserva el retorno para el error.
- `channel_modes()` acota lo que escribe en `mbuf`: antes era correcto por
  accidente (22 letras), y con registro dinámico deja de serlo.

El único cambio observable es el orden de las letras dentro de una cadena de
modos, que ahora es el del alfabeto (`A C D M R U Z b c d i k l m n o p r s t u
v z`). El orden nunca fue significativo, y a cambio es el mismo en todos los
servidores.

## 6. Reparto en commits

| # | Commit | Contenido |
|---|---|---|
| 1 | `feat(modes): channel modes refactor` | `chanmode_t`, `chan_flags.h`, bits de control arriba, registro en `chan_modes.c`, las cinco tablas colapsadas, MYINFO y CHANMODES construidos |
| 2 | `feat(module): channel modes from module` | `module_add_chan_mode()` y familia, reversión al descargar, `/STATS M`, ABI 3 |
| 3 | `feat(hooks): dispatch HOOK_CHANNEL_PRE_MODE` | despacho del hook, módulo de ejemplo y documentación |

El commit 1 no cambia comportamiento observable salvo el orden de las letras: es
el que hay que poder revisar y revertir por separado.

## 7. Pruebas

**Unitarias** (`ircd/test/`): `chan_modes_t.c` + `chan_modes_stub.c` cubren que el
bit sale de la letra y de nada más, que las letras se rechazan si no son letras o
si ya están tomadas, que la lista sale ordenada por bit, que quitar un modo lo
borra de los canales que lo tenían y lo anuncia a los miembros **y** a la red, y
que MYINFO y CHANMODES salen del registro (con y sin `FEAT_OPLEVELS`).
`module_t.c` se amplía con el fixture `modules/mod_cmode.c`: alta, baja explícita,
50 ciclos de carga/descarga y anuncio.

**Integración** (`tests/`, pytest sobre Docker): pendiente — un caso que cargue
`cmode_nocaps`, compruebe que `+G` se acepta, aparece en `MODE`, en `RPL_MYINFO` y
en `CHANMODES`, que el otro servidor de la topología lo ve, y que
`/MODULE UNLOAD` deja el canal sin `+G` en ambos.

## 8. Correspondencia con los modos de usuario

| Modos de usuario (ABI 2) | Modos de canal (ABI 3) |
|---|---|
| `flag_t` (`include/user_flags.h`) | `chanmode_t` (`include/chan_flags.h`) |
| `struct UserMode`, `UserModeList` | `struct ChanMode`, `ChanModeList` |
| `client_init_user_modes()` | `channel_init_chan_modes()` |
| `client_find_user_mode()` | `channel_find_chan_mode()` |
| `client_alloc_user_mode_flag()` (reparte) | `channel_chan_mode_flag()` (deriva de la letra) |
| `client_append_user_mode()` / `client_remove_user_mode()` | `channel_append_chan_mode()` / `channel_remove_chan_mode()` |
| `client_user_mode_chars()` → `RPL_MYINFO` | `channel_chan_mode_chars()` → `RPL_MYINFO`; `channel_chanmodes_supported()` → `ISUPPORT` |
| `HasUFlag` / `SetUFlag` / `ClrUFlag` | `HasCFlag` / `SetCFlag` / `ClrCFlag` |
| `module_add_user_mode()` | `module_add_chan_mode()` |
| `HOOK_CLIENT_PRE_UMODE` | `HOOK_CHANNEL_PRE_MODE` |
| `modules/umode_nopm.c` (`+P`) | `modules/cmode_nocaps.c` (`+G`) |
