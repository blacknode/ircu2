# Plan de trabajo — API de módulos cargables

**Documento de diseño:** [`001-module-api.md`](001-module-api.md)
**Rama base:** `main` (`3584504`, con CMake ya integrado)
**Estado:** pendiente de aprobación
**Fecha:** 2026-09-08

---

## 0. Resumen para aprobar

| | |
|---|---|
| Fases | 7, cada una un PR independiente |
| Entregable útil más temprano | Fase 3 (comandos desde `.so`, sin tocar el core) |
| Entregable completo | Fase 6 (hooks con veto en todos los puntos) |
| Ficheros nuevos en el core | 4 (`module.h/.c`, `hooks.h/.c`) |
| Ficheros del core modificados | ~12, casi todos con 2–4 líneas |
| Decisiones que requieren tu aprobación | 5 (§7) |

**Se pide aprobar:** el alcance, el orden de fases, y las 5 decisiones abiertas de §7.

---

## 1. Validación previa ya realizada

Antes de escribir este plan se verificó el supuesto técnico que sostiene todo lo
demás: que un `.so` puede resolver símbolos del ejecutable `ircd` en tiempo de
ejecución.

**Prototipo aislado** — un host con `-rdynamic` cargando un `.so` que referencia
símbolos del host sin enlazar contra nada: carga, resuelve, ejecuta, muta estado del
host, y descarga limpiamente.

**Sobre el binario real** — compilando `ircd` con `-rdynamic` sobre el árbol actual:

```
build OK
add_msg_element      SI      exit_client   SI
register_mapping     SI      DoMalloc      SI
send_reply           SI
total de símbolos exportados: 890
```

Dos consecuencias que el plan aprovecha:

1. `add_msg_element()` **no es `static`** (`ircd/parse.c:742`) — sólo le falta
   declaración en una cabecera pública. No hay que refactorizar `parse.c`.
2. Los 890 símbolos exportados cubren de sobra lo que un módulo necesita.

Riesgo técnico principal: **descartado**.

---

## 2. Fases

Cada fase es un PR revisable por separado, con criterio de aceptación propio. El
orden está elegido para que cada una deje el árbol en estado útil y comprobable.

---

### Fase 1 — Infraestructura de carga

**Objetivo:** cargar y descargar un `.so` sin que registre nada todavía.

**Ficheros nuevos**
- `include/module.h` — `struct ModuleInfo`, `struct ModuleHandle`, `IRCU_MODULE_ABI`
- `ircd/module.c` — `dlopen`/`dlsym`/`dlclose`, lista de módulos cargados, validación de ABI

**Ficheros modificados**
- `ircd/CMakeLists.txt` — `ENABLE_EXPORTS ON`, `${CMAKE_DL_LIBS}`
- `ircd/ircd.c` — `module_init()` al arrancar, `module_shutdown()` al salir

**Criterio de aceptación**
- Con cero módulos configurados, el servidor se comporta **exactamente** como hoy
- Un `.so` con ABI incorrecto se rechaza y se registra en log, sin caída
- Un `.so` inexistente o corrupto se rechaza y se registra en log, sin caída
- `valgrind` limpio en carga + descarga de un módulo trivial

**Tamaño:** ~350 líneas. **Riesgo:** bajo.

---

### Fase 2 — Configuración y comando `/MODULE`

**Objetivo:** administrar módulos desde `ircd.conf` y en caliente.

**Ficheros modificados**
- `ircd/ircd_parser.y` + `ircd/ircd_lexer.c` — bloque `Module { path = "..."; }`
- `ircd/s_conf.c` — ciclo de vida en `/REHASH` (cargar / descargar / recargar por `mtime`)
- `include/client.h` — `PRIV_MODULE` (6 sitios a tocar, medido sobre `PRIV_LOCAL_BADCHAN`)
- `ircd/m_module.c` *(nuevo)* — `LIST`, `LOAD`, `UNLOAD`, `RELOAD`
- `ircd/parse.c` — entrada en `msgtab`
- `include/msg.h` — `MSG_MODULE` / `TOK_MODULE`
- `doc/example.conf` — bloque documentado

**Criterio de aceptación**
- `/MODULE LIST` muestra nombre, versión, ruta y ABI
- `/REHASH` carga los nuevos, descarga los que desaparecieron, recarga los que cambiaron
- Sin `PRIV_MODULE` → `ERR_NOPRIVILEGES`; con `FEAT_CONFIG_OPERCMDS` en `FALSE` → `ERR_DISABLED`
- Recargar 100 veces seguidas no filtra memoria

**Tamaño:** ~450 líneas. **Riesgo:** bajo-medio (el parser de config es delicado).

---

### Fase 3 — Registro de comandos ⭐

**Objetivo:** un módulo puede añadir comandos. **Primera entrega con valor real.**

**Ficheros modificados**
- `include/parse.h` — declarar `add_msg_element()` / `del_msg_element()` / `msg_tree_parse()`
- `ircd/module.c` — `module_add_command()` / `module_del_command()`
- `include/module.h` — API pública

**Ficheros nuevos**
- `modules/example_cmd/` — comando de ejemplo + su `CMakeLists.txt`
- `cmake/IrcuModules.cmake` — función `ircu_add_module()`

**Detalle de implementación:** el mismo camino que `register_mapping()`
(`ircd/parse.c:831`) — construir `struct Message` en heap, rellenar el vector de
handlers, `add_msg_element()`. El `ModuleHandle` guarda la lista para revertirla en
la descarga aunque el módulo no lo haga.

**Criterio de aceptación**
- Comando registrado por un módulo responde a un cliente real
- Colisión con comando existente → registro rechazado, log, módulo **no** cargado
- Descargar el módulo quita el comando del trie; invocarlo después → `ERR_UNKNOWNCOMMAND`
- Cargar → descargar → cargar 50 veces sin fuga ni corrupción del trie

**Tamaño:** ~300 líneas. **Riesgo:** bajo — el mecanismo ya está probado en producción por los pseudo-comandos.

---

### Fase 4 — Motor de hooks (sin puntos de inserción)

**Objetivo:** la maquinaria de cadenas de hooks, probada de forma aislada.

**Ficheros nuevos**
- `include/hooks.h` — `enum HookType`, `enum HookResult`, `struct HookContext`
- `ircd/hooks.c` — cadenas por tipo, orden por prioridad, despacho
- `ircd/test/hooks_t.c` — prueba unitaria bajo CTest

**Ficheros modificados**
- `ircd/module.c` — `module_add_hook()` / `module_del_hook()`
- `ircd/test/CMakeLists.txt` — registrar `hooks_t`

**Criterio de aceptación** (todo verificable sin levantar el servidor)
- Orden por prioridad respetado; empates por orden de carga
- `HOOK_CONTINUE` sigue la cadena; `ALLOW` y `DENY` la cortan
- Un `DENY` no puede revertirse por un hook posterior
- Quitar un hook desde dentro de su propia ejecución no corrompe la cadena
- Descargar un módulo retira todos sus hooks

**Tamaño:** ~400 líneas (la mitad, pruebas). **Riesgo:** bajo — no toca el core.

---

### Fase 5 — Puntos de inserción: notificación

**Objetivo:** hooks que sólo observan. Sin veto, riesgo mínimo.

| Hook | Fichero | Punto |
|---|---|---|
| `CLIENT_REGISTERED` | `ircd/s_user.c` | `register_user()` (`:341`) |
| `CLIENT_NICK_CHANGED` | `ircd/s_user.c` | `set_nick_name()` (`:527`) |
| `CLIENT_EXITING` | `ircd/s_misc.c` | `exit_client()` (`:380`) |
| `CHANNEL_JOINED` | `ircd/channel.c` | `add_user_to_channel()` (`:543`) |
| `CHANNEL_PARTED` | `ircd/channel.c` | `remove_user_from_channel()` (`:660`) |
| `SERVER_LINKED` | `ircd/s_serv.c` | `server_estab()` (`:122`) |

**Criterio de aceptación**
- Sin módulos cargados, el coste es una comparación de puntero nulo por punto
- Un módulo de trazas registra los seis eventos con los datos correctos
- La suite de integración de `tests/` pasa sin cambios

**Tamaño:** ~150 líneas (2–4 por punto). **Riesgo:** bajo.

---

### Fase 6 — Puntos de inserción: veto ⭐

**Objetivo:** la funcionalidad que se pidió — decidir si un proceso continúa.

| Hook | Fichero | Punto |
|---|---|---|
| `CLIENT_PRE_NICK` | `ircd/m_nick.c` | tras `do_nick_name()` (`:179`), antes de `set_nick_name()` |
| `CLIENT_PRE_REGISTER` | `ircd/s_user.c` | `register_user()`, antes de anunciar |
| `CLIENT_PRE_UMODE` | `ircd/s_user.c` | `set_user_mode()` (`:1023`) |
| `CHANNEL_PRE_JOIN` | `ircd/m_join.c` | junto a `ERR_BANNEDFROMCHAN` (`:173`) |
| `CHANNEL_PRE_CREATE` | `ircd/channel.c` | `get_channel()` con `CGT_CREATE` (`:1354`) |
| `MESSAGE_PRE_CHANNEL` | `ircd/ircd_relay.c` | `relay_channel_message()` (`:87`) |
| `MESSAGE_PRE_PRIVATE` | `ircd/ircd_relay.c` | `relay_private_message()` (`:479`) |

**Un commit por punto de inserción**, cada uno con su prueba. Es la fase más
delicada del proyecto y conviene poder revertir uno sin tocar los demás.

**Invariante que se verifica en cada commit:** el hook corre **sólo** en el camino
del cliente local (`m_*`), nunca en el de propagación entre servidores (`ms_*`).
Vetar algo que el resto de la red ya aceptó desincroniza el estado.

**Módulos de ejemplo entregados con esta fase**
- `modules/nickpolicy/` — veta cambios de nick según política
- `modules/nocaps/` — rechaza mensajes en mayúsculas (el caso del documento de diseño)

**Criterio de aceptación**
- Cada punto: prueba de integración en `tests/` que verifica veto y permiso
- Un `DENY` produce el numérico correcto y **no** deja estado a medias
- Un `DENY` en `PRE_JOIN` no deja al usuario en el canal en ningún servidor
- La suite completa pasa con y sin módulos cargados

**Tamaño:** ~500 líneas repartidas en 7 commits + pruebas. **Riesgo: alto** — es
donde se puede desincronizar la red.

---

### Fase 7 — Documentación y endurecimiento

**Ficheros nuevos**
- `doc/readme.modules` — guía de autor de módulos, ABI, reglas, ejemplo comentado

**Ficheros modificados**
- `ircd/s_stats.c` — `/STATS` de módulos: cargados, hooks por tipo, invocaciones
- `.github/workflows/build.yml` — compilar los módulos de ejemplo en CI

**Criterio de aceptación**
- Alguien ajeno al proyecto escribe un módulo siguiendo sólo la documentación
- CI compila los módulos de ejemplo en las tres configuraciones de la matriz

**Tamaño:** ~300 líneas, casi todo documentación. **Riesgo:** bajo.

---

## 3. Secuencia y puntos de corte

```
Fase 1 ──▶ Fase 2 ──▶ Fase 3 ──▶ [ÚTIL: comandos en .so]
                          │
                          ▼
                       Fase 4 ──▶ Fase 5 ──▶ Fase 6 ──▶ Fase 7
                                                 │
                                          [COMPLETO: veto]
```

Dos puntos de corte legítimos:

- **Tras la fase 3** — ya se pueden distribuir comandos nuevos sin tocar el core. Si
  el proyecto se para aquí, lo entregado es coherente y útil.
- **Tras la fase 5** — hooks de observación (métricas, auditoría, logs) sin ninguno
  de los riesgos del veto.

---

## 4. Estrategia de pruebas

**Unitarias (CTest, `ircd/test/`)** — para la lógica que no necesita servidor:
`hooks_t.c` cubre orden, corte, retirada durante ejecución y limpieza al descargar.
Se suma a los 6 tests existentes.

**Integración (pytest + Docker, `tests/`)** — para todo lo observable desde un
cliente: un módulo de prueba compilado en la imagen, y casos que verifican veto y
permiso en cada punto de la fase 6.

**Fugas y corrupción** — un objetivo de CTest que carga y descarga en bucle bajo
valgrind. Es la prueba que atrapa la clase de bug más probable de esta API.

**No regresión** — toda la suite existente debe pasar con cero módulos cargados. Ese
es el criterio que protege a quien no use módulos.

---

## 5. Riesgos, con dueño

| Riesgo | Fase | Mitigación |
|---|---|---|
| Uso tras liberar al descargar | 1, 3, 4 | `ModuleHandle` revierte todo lo registrado; prohibido descargar desde dentro de un hook (asserción) |
| Corrupción del trie de comandos | 3 | Prueba de carga/descarga en bucle; rechazo de colisiones |
| Desincronización de la red por veto | 6 | Un commit por punto; invariante `m_*` verificado en cada uno |
| Deriva de ABI silenciosa | 1 | Comparación exacta; rechazo con log |
| Hook lento congela el event loop | 4, 7 | Contador de tiempo por hook en `/STATS`; documentado |
| Un módulo tumba el servidor | — | **Sin mitigación técnica.** Comparte espacio de direcciones, incluidas las claves TLS. Es política operativa, y va en grande en `doc/readme.modules` |

---

## 6. Fuera de alcance (v1)

- Dependencias entre módulos y orden de carga garantizado
- Que un módulo defina privilegios o features nuevos
- Hooks asíncronos (ver §7, decisión 5)
- Sandboxing o aislamiento de módulos
- Hooks en el camino de servidor (`ms_*`) — prohibido por diseño
- ABI estable entre versiones del servidor

---

## 7. Decisiones que requieren tu aprobación

**1. Política de ABI.** ¿Se congela por rama estable, o se rompe libremente entre
releases? *Recomendación: romper libremente en v1; los módulos se recompilan.
Congelarlo demasiado pronto ata el diseño antes de saber si es el correcto.*

**2. Estado entre recargas.** ¿El core ofrece un mecanismo para conservar estado al
recargar un módulo? *Recomendación: no en v1. El módulo persiste lo que necesite.*

**3. Privilegios propios de módulos.** ¿Pueden definir `PRIV_*` nuevos? *Recomendación:
no en v1 — implica tocar el parser de configuración. Que usen los existentes.*

**4. Ubicación de los módulos de ejemplo.** ¿`modules/` en la raíz, o
`contrib/modules/`? *Recomendación: `modules/` en la raíz, coherente con `tools/`.*

**5. Hooks asíncronos — la decisión con fecha límite.** Un módulo que quiera hacer
trabajo bloqueante (consultar una API antes de dejar pasar un mensaje) necesita
suspender la operación y reanudarla. Eso es un `HOOK_PENDING` que la propuesta no
contempla, **y añadirlo después rompe el ABI**.

Hay dos salidas y conviene elegir antes de la fase 4:

- **(a)** Reservar el valor ahora en `enum HookResult` sin implementarlo. Coste hoy:
  una línea. Deja la puerta abierta.
- **(b)** Decidir que los hooks son siempre síncronos, y que el trabajo bloqueante va
  por la propuesta 002 (workers).

*Recomendación: (a).* Cuesta una línea y evita una rotura de ABI previsible.

---

## 8. Qué necesito para empezar

1. Aprobación del alcance y el orden de fases
2. Respuesta a las 5 decisiones de §7 (o conformidad con las recomendaciones)
3. Confirmación de si se busca el corte tras la fase 3, tras la 5, o el plan completo
