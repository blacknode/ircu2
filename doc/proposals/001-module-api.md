# Propuesta 001 — API de módulos cargables (`.so` vía `dlopen`)

**Estado:** borrador para revisión
**Rama base:** `main` (`0200c5d`)
**Alcance:** extender ircu con comandos y hooks de ciclo de vida sin recompilar el servidor

---

## 1. Objetivo

Permitir que funcionalidad nueva se distribuya como objetos compartidos (`.so`) que el
servidor carga en tiempo de ejecución, con dos capacidades:

1. **Comandos nuevos** — registrar entradas en la tabla de comandos con sus propios
   handlers por tipo de cliente.
2. **Hooks de ciclo de vida** — interceptar puntos del ciclo de vida de usuarios y
   canales, **con capacidad de veto**: el hook decide si el proceso continúa
   (¿puede este usuario cambiar de nick? ¿puede usar mayúsculas? ¿puede entrar a este
   canal?).

El objetivo secundario, y no menor, es **dejar de forzar forks del core** para cada
política de red particular.

---

## 2. Punto de partida: lo que ya existe

Esta propuesta no parte de cero. El árbol ya contiene tres piezas que la hacen
mucho más barata de lo que parece.

### 2.1 Registro de comandos en tiempo de ejecución — ya implementado

`ircd/parse.c` mantiene los comandos en dos tries (`msg_tree` por nombre, `tok_tree`
por token P10) y expone funciones para insertar y quitar nodos **en caliente**:

```c
static void add_msg_element(struct MessageTree *mtree_p, struct Message *msg_p, char *cmd);
struct MessageTree *del_msg_element(struct MessageTree *mtree_p, char *cmd);
```

Y ya hay un consumidor de esas funciones: los pseudo-comandos de servicios
(`register_mapping()` / `unregister_mapping()`, `ircd/parse.c:831`), que construyen
un `struct Message` en el heap y lo insertan en el trie durante un rehash:

```c
msg = (struct Message *)MyMalloc(sizeof(struct Message));
msg->cmd = map->command;
msg->tok = map->command;
msg->parameters = 2;
msg->flags = MFLG_EXTRA;
msg->extra = map;
msg->handlers[CLIENT_HANDLER] = m_pseudo;
/* ... */
add_msg_element(&msg_tree, msg, msg->cmd);
```

**El mecanismo de registro dinámico de comandos ya está probado en producción.** Un
módulo hace exactamente lo mismo, con sus propios punteros a función en lugar de
`m_pseudo`.

### 2.2 Despacho por tipo de cliente — ya modelado

`struct Message` (`include/msg.h:414`) lleva un vector de handlers indexado por
`HandlerType` (`include/ircd_handler.h:33`):

```
UNREGISTERED_HANDLER, CLIENT_HANDLER, SERVER_HANDLER, OPER_HANDLER, SERVICE_HANDLER
```

con la firma uniforme:

```c
typedef int (*MessageHandler)(struct Client* cptr, struct Client* sptr,
                              int parc, char* parv[]);
```

Un módulo no necesita ninguna abstracción nueva: rellena ese vector.

### 2.3 Convención de carga de código externo — ya existe el precedente

El servidor ya lanza y gestiona código de terceros: el helper iauth
(`fork` + `execvp`, `ircd/s_auth.c:1683`), configurado por bloque `IAuth {}` y
recargable en rehash. La gestión de ciclo de vida de módulos puede seguir el mismo
patrón de configuración y las mismas convenciones de `/STATS`.

---

## 3. Diseño propuesto

### 3.1 Archivos nuevos

```
include/module.h        API pública que ve el módulo (ABI estable)
ircd/module.c           cargador: dlopen/dlsym/dlclose, registro, rehash
include/hooks.h         catálogo de hooks y tipos de retorno
ircd/hooks.c            cadenas de hooks, prioridades, despacho
```

Y un directorio de módulos de ejemplo:

```
modules/example_cmd/    comando trivial
modules/nickpolicy/     hook de veto de cambio de nick
```

### 3.2 Contrato del módulo

Cada `.so` exporta **un único símbolo**, una estructura de descripción. Nada más;
todo lo demás se alcanza a través de punteros dentro de ella. Esto mantiene la
superficie de ABI pequeña y auditable.

```c
/* include/module.h */

#define IRCU_MODULE_ABI 1

struct ModuleInfo {
  unsigned int  mi_abi;        /* IRCU_MODULE_ABI con el que se compiló */
  const char   *mi_name;       /* "nickpolicy" */
  const char   *mi_version;    /* "1.0.0" */
  const char   *mi_author;
  const char   *mi_description;

  /* Llamado tras dlopen. Devuelve 0 si el módulo queda cargado. */
  int  (*mi_init)(struct ModuleHandle *mod);

  /* Llamado antes de dlclose. Debe deshacer TODO lo registrado. */
  void (*mi_fini)(struct ModuleHandle *mod);

  /* Opcional: reaccionar a /REHASH sin descargar. */
  void (*mi_rehash)(struct ModuleHandle *mod);
};

/* El único símbolo que el .so debe exportar. */
extern struct ModuleInfo ircu_module;
```

El cargador rechaza cualquier módulo cuyo `mi_abi` no coincida exactamente con el
del servidor. Sin negociación de versiones, sin compatibilidad hacia atrás: si el
ABI cambia, los módulos se recompilan. En un servidor donde un puntero mal
interpretado es una caída de toda la red, esa rigidez es una virtud.

### 3.3 Registro de comandos

```c
/* Todo registro se hace contra el ModuleHandle, para poder revertirlo. */
int module_add_command(struct ModuleHandle *mod,
                       const char *cmd,          /* "SPAMFILTER" */
                       const char *tok,          /* token P10, o NULL */
                       unsigned int min_parc,
                       unsigned int flags,       /* MFLG_* */
                       MessageHandler handlers[LAST_HANDLER_TYPE]);

int module_del_command(struct ModuleHandle *mod, const char *cmd);
```

Implementación: construir un `struct Message` y llamar a `add_msg_element()`, igual
que `register_mapping()`. El `ModuleHandle` guarda la lista de comandos registrados
para revertirlos en `mi_fini` aunque el módulo se olvide de hacerlo.

**Reglas de colisión.** `module_add_command()` falla si el nombre ya existe —
comprobación con `msg_tree_parse()`, como ya hace `register_mapping()`. Los módulos
no pueden sustituir comandos del core. Sobrescribir `PRIVMSG` desde un `.so` es
exactamente el tipo de cosa que convierte un servidor en algo indepurable.

### 3.4 Hooks: el catálogo

Los hooks se declaran como un enum cerrado en el core. Un módulo no puede inventar
puntos de intercepción; elige de una lista que el core garantiza y documenta.

```c
/* include/hooks.h */

enum HookType {
  /* --- ciclo de vida del cliente --- */
  HOOK_CLIENT_PRE_REGISTER,   /* veto: ¿se le permite completar el registro? */
  HOOK_CLIENT_REGISTERED,     /* notificación */
  HOOK_CLIENT_PRE_NICK,       /* veto: ¿puede usar este nick? */
  HOOK_CLIENT_NICK_CHANGED,   /* notificación */
  HOOK_CLIENT_PRE_QUIT,       /* veto sobre el mensaje, no sobre la salida */
  HOOK_CLIENT_EXITING,        /* notificación */
  HOOK_CLIENT_PRE_UMODE,      /* veto: ¿puede ponerse este modo? */

  /* --- ciclo de vida del canal --- */
  HOOK_CHANNEL_PRE_CREATE,    /* veto: ¿puede crear este canal? */
  HOOK_CHANNEL_PRE_JOIN,      /* veto: ¿puede entrar? */
  HOOK_CHANNEL_JOINED,        /* notificación */
  HOOK_CHANNEL_PRE_PART,
  HOOK_CHANNEL_PARTED,
  HOOK_CHANNEL_PRE_MODE,      /* veto sobre un cambio de modo concreto */
  HOOK_CHANNEL_PRE_TOPIC,
  HOOK_CHANNEL_DESTROYED,

  /* --- mensajería --- */
  HOOK_MESSAGE_PRE_CHANNEL,   /* veto y/o reescritura del texto */
  HOOK_MESSAGE_PRE_PRIVATE,

  /* --- red --- */
  HOOK_SERVER_LINKED,
  HOOK_SERVER_SPLIT,

  HOOK_LAST
};
```

### 3.5 Hooks: el contrato de veto

Éste es el punto donde la propuesta se juega su utilidad, así que conviene ser
explícito.

```c
enum HookResult {
  HOOK_CONTINUE,   /* no opino, que siga la cadena */
  HOOK_ALLOW,      /* permitir y CORTAR la cadena (hooks posteriores no corren) */
  HOOK_DENY        /* denegar y cortar; el core aborta la operación */
};

struct HookContext {
  struct Client  *hc_client;   /* sujeto de la acción */
  struct Client  *hc_source;   /* origen del mensaje (puede diferir) */
  struct Channel *hc_channel;  /* si aplica */

  const char     *hc_arg;      /* nick propuesto, texto, modo... según hook */
  char           *hc_rewrite;  /* buffer opcional para reescritura */
  size_t          hc_rewrite_len;

  /* Cómo el módulo explica un DENY. */
  int             hc_numeric;  /* numérico a enviar; 0 = usar el del core */
  char            hc_reason[TOPICLEN + 1];
};

typedef enum HookResult (*HookFn)(struct HookContext *ctx, void *user);

int module_add_hook(struct ModuleHandle *mod, enum HookType type,
                    HookFn fn, int priority, void *user);
int module_del_hook(struct ModuleHandle *mod, enum HookType type, HookFn fn);
```

**Semántica de la cadena.** Los hooks de un tipo se ordenan por `priority`
(menor corre antes; empates por orden de carga). El core recorre la cadena hasta
que alguien devuelve algo distinto de `HOOK_CONTINUE`. Si nadie opina, se aplica
la política por defecto del core.

**Un `DENY` es soberano.** No hay forma de que un hook posterior revierta un
`HOOK_DENY`. La alternativa —permitir que el último en la cadena gane— hace que el
resultado dependa del orden de carga de los módulos, que es precisamente lo que
nadie quiere depurar a las tres de la mañana.

**Reescritura.** Sólo los hooks marcados como reescribibles
(`HOOK_MESSAGE_PRE_*`, `HOOK_CLIENT_PRE_NICK`) leen `hc_rewrite`. El módulo escribe
en el buffer que el core le proporciona; no asigna memoria que el core deba liberar.

### 3.6 Ejemplo completo: el caso "no puede usar mayúsculas"

```c
#include "module.h"
#include "hooks.h"
#include <ctype.h>
#include <string.h>

static enum HookResult
deny_shouting(struct HookContext *ctx, void *user)
{
  size_t i, upper = 0, alpha = 0, len = strlen(ctx->hc_arg);

  if (len < 10)
    return HOOK_CONTINUE;          /* mensajes cortos, sin opinión */

  for (i = 0; i < len; i++) {
    if (isalpha((unsigned char)ctx->hc_arg[i])) {
      alpha++;
      if (isupper((unsigned char)ctx->hc_arg[i]))
        upper++;
    }
  }

  if (alpha && (upper * 100 / alpha) > 70) {
    ctx->hc_numeric = ERR_CANNOTSENDTOCHAN;
    strncpy(ctx->hc_reason, "Baja el volumen", sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }

  return HOOK_CONTINUE;
}

static int mod_init(struct ModuleHandle *mod)
{
  return module_add_hook(mod, HOOK_MESSAGE_PRE_CHANNEL, deny_shouting, 100, NULL);
}

static void mod_fini(struct ModuleHandle *mod)
{
  /* el cargador revierte los registros automáticamente; esto es por claridad */
  module_del_hook(mod, HOOK_MESSAGE_PRE_CHANNEL, deny_shouting);
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "nocaps", "1.0.0", "you", "Rechaza mensajes en mayúsculas",
  mod_init, mod_fini, NULL
};
```

---

## 4. Puntos de inserción en el core

Cada hook necesita una llamada en un sitio concreto. Éstos son los identificados,
con el fichero y la función donde iría:

| Hook | Fichero | Función / punto |
|---|---|---|
| `CLIENT_PRE_REGISTER` | `ircd/s_user.c` | `register_user()`, antes de anunciar |
| `CLIENT_PRE_NICK` | `ircd/m_nick.c` | tras `do_nick_name()`, antes de `set_nick_name()` |
| `CLIENT_NICK_CHANGED` | `ircd/s_user.c` | `set_nick_name()`, tras propagar |
| `CLIENT_EXITING` | `ircd/s_misc.c` | `exit_client()` |
| `CLIENT_PRE_UMODE` | `ircd/s_user.c` | `set_user_mode()` |
| `CHANNEL_PRE_JOIN` | `ircd/m_join.c` | junto a la comprobación `ERR_BANNEDFROMCHAN` (~línea 173) |
| `CHANNEL_JOINED` | `ircd/channel.c` | `add_user_to_channel()` |
| `CHANNEL_PARTED` | `ircd/channel.c` | `remove_user_from_channel()` |
| `CHANNEL_PRE_CREATE` | `ircd/channel.c` | `get_channel()` con `CGT_CREATE` |
| `MESSAGE_PRE_CHANNEL` | `ircd/ircd_relay.c` | `relay_channel_message()`, tras `client_can_send_to_channel()` |
| `MESSAGE_PRE_PRIVATE` | `ircd/ircd_relay.c` | `relay_private_message()` |
| `SERVER_LINKED` | `ircd/s_serv.c` | `server_estab()` |

**Regla de oro: los hooks sólo corren para clientes locales.** Un hook que vete una
acción ya aceptada por otro servidor de la red produce desincronización de estado —
el resto de la red cree que el usuario cambió de nick y este servidor no. Los puntos
de veto se colocan siempre en el camino `m_*` (cliente local), nunca en `ms_*`
(propagación entre servidores). Esto es una restricción del diseño, no un detalle de
implementación, y conviene documentarla en la cabecera.

---

## 5. Configuración

Bloque nuevo en `ircd.conf`, siguiendo la forma del bloque `IAuth`:

```
Module {
 path = "/opt/ircu/lib/modules/nocaps.so";
};

Module {
 path = "/opt/ircu/lib/modules/spamfilter.so";
 # opcional: parámetros que llegan al módulo en mi_init
 option = "threshold=70";
};
```

Comportamiento en `/REHASH`:

- módulo en la config nueva y no cargado → cargar
- cargado y ya no está en la config → `mi_fini` + `dlclose`
- en ambas y el `mtime` del fichero cambió → recargar (descargar + cargar)
- en ambas y sin cambios → llamar a `mi_rehash` si existe

Y comandos de operador: `/MODULE LIST`, `/MODULE LOAD <path>`,
`/MODULE UNLOAD <name>`, `/MODULE RELOAD <name>` — sujetos a un privilegio nuevo
`PRIV_MODULE`, y a `FEAT_CONFIG_OPERCMDS` como el resto de comandos que tocan la
configuración.

---

## 6. Construcción

Depende de la migración a CMake (rama `claude/autotools-cmake-migration-6uod0q`).
Con ella:

```cmake
# El ircd debe exportar sus símbolos para que los módulos los resuelvan
set_target_properties(ircd PROPERTIES ENABLE_EXPORTS ON)

function(ircu_add_module name)
  add_library(${name} MODULE ${ARGN})
  target_link_libraries(${name} PRIVATE ircu_config)
  set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".so")
  install(TARGETS ${name} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/ircu/modules")
endfunction()
```

Y `target_link_libraries(ircd PRIVATE ${CMAKE_DL_LIBS})` para `dlopen`.

Los módulos resuelven los símbolos del core contra el ejecutable, no contra una
librería compartida. Eso exige `-rdynamic` (`ENABLE_EXPORTS`), y significa que
**toda función no `static` del ircd queda visible para los módulos**. Es la vía
simple, y es lo que hacen las implementaciones equivalentes en otros ircd.

---

## 7. Riesgos y cómo se acotan

| Riesgo | Mitigación |
|---|---|
| Un módulo con un bug tumba el servidor | No hay mitigación técnica real: comparte espacio de direcciones. Se acota con revisión, no con código. **Documentarlo en grande.** |
| `dlclose` con punteros vivos → uso tras liberar | El `ModuleHandle` rastrea todo lo registrado y lo revierte antes del `dlclose`. Además: nunca descargar desde dentro de un hook. |
| Deriva de ABI silenciosa | `mi_abi` exacto; el cargador rechaza y registra en log. |
| Un hook lento congela el event loop | Documentar el presupuesto (los hooks corren en el hilo principal); considerar un contador de tiempo por hook expuesto en `/STATS`. |
| Módulos que dependen unos de otros | Fuera de alcance en v1. Sin orden de carga garantizado más allá del de la config. |
| Reescritura de mensajes con desbordamiento | El core proporciona el buffer y su tamaño; el módulo nunca asigna. |

Un riesgo que **no** se puede acotar y conviene decir en voz alta: esto convierte
código de terceros en parte del servidor. Un `.so` malicioso o simplemente
descuidado tiene acceso completo a la memoria del proceso, incluidas las claves TLS.
La política de qué se carga es una decisión operativa, no técnica.

---

## 8. Plan de trabajo

| Fase | Contenido | Tamaño estimado |
|---|---|---|
| 1 | `module.h` + `module.c`: dlopen/dlsym/dlclose, `ModuleHandle`, bloque de config, `/MODULE LIST` | pequeña |
| 2 | Registro de comandos sobre `add_msg_element()` + módulo de ejemplo | pequeña |
| 3 | `hooks.h` + `hooks.c`: cadenas, prioridades, despacho | mediana |
| 4 | Insertar los puntos de la tabla §4, uno por commit, con prueba | mediana, la más delicada |
| 5 | `/MODULE LOAD/UNLOAD/RELOAD`, privilegio, integración con rehash | pequeña |
| 6 | Pruebas: unitarias de la cadena de hooks + integración en `tests/` | mediana |

Las fases 1 y 2 son entregables por sí solas: con ellas ya se pueden escribir
comandos nuevos sin tocar el core. La 3 y la 4 son las que dan el veto.

---

## 9. Preguntas abiertas

1. **¿Hooks para servidores remotos?** La propuesta los prohíbe (§4). ¿Hay algún caso
   de uso que lo justifique aceptando el riesgo de desincronización?
2. **¿Persistencia de estado entre recargas?** Un módulo que recarga pierde su
   estado en memoria. ¿Hace falta un mecanismo del core para conservarlo, o es
   problema del módulo?
3. **¿Qué pasa con `PRIV_*`?** ¿Los módulos pueden definir privilegios nuevos, o se
   conforman con los existentes? Definirlos implica tocar el parser de config.
4. **Versión del ABI y política de rotura.** ¿Se congela el ABI por rama estable, o
   se acepta romperlo en cualquier release?
