# Propuesta 004 — Configuración desde el entorno

**Estado:** propuesta, con implementación de referencia
**Depende de:** nada (no toca el API de módulos ni la ABI)
**Introduce:** `include/ircd_env.h`, `ircd/ircd_env.c`

## 1. Motivación

Un ircd que se despliega en un contenedor no trae su configuración en la
imagen: el nombre del servidor, el numérico, la contraseña de enlace y el hash
del oper llegan desde fuera, y la forma universal de pasarlos es el entorno del
proceso — es lo que hablan docker-compose, systemd (`EnvironmentFile=`),
Kubernetes (`env`, `envFrom`, secretos montados como variables) y cualquier
orquestador.

`ircd.conf` no sabe nada del entorno. Hoy el hueco se tapa fuera del servidor,
siempre igual y siempre mal (§2). Lo que falta es que esto funcione:

```
General {
        name = "${IRCD_NAME}";
        numeric = ${IRCD_NUMERIC:-1};
};

Connect {
        name = "hub.example.net";
        password = "${IRCD_LINK_PASS:?ponga la contraseña de enlace}";
};
```

y que el código C tenga una forma de leer una variable con un valor por defecto
y un rango sin repetir `getenv()` + `strtol()` + un aviso en cada sitio.

## 2. Cómo se hace hoy: código legacy de ejemplo

El patrón habitual — el de este repositorio incluido, donde
`tests/docker/ircd-entrypoint.sh` ya lee `IRCD_DEBUG` del entorno — es plantillar
el fichero de configuración antes de arrancar:

```sh
#!/bin/sh
# entrypoint.sh (legacy): rellena la plantilla y arranca
set -eu

sed -e "s/@@NAME@@/${IRCD_NAME}/g" \
    -e "s/@@NUMERIC@@/${IRCD_NUMERIC}/g" \
    -e "s/@@LINK_PASS@@/${IRCD_LINK_PASS}/g" \
    /opt/ircu/lib/ircd.conf.tmpl > /opt/ircu/lib/ircd.conf

exec /opt/ircu/bin/ircd -f /opt/ircu/lib/ircd.conf -n
```

o su variante con `envsubst`:

```sh
envsubst < /opt/ircu/lib/ircd.conf.tmpl > /opt/ircu/lib/ircd.conf
exec /opt/ircu/bin/ircd -f /opt/ircu/lib/ircd.conf -n
```

Cinco problemas, todos reales:

1. **El secreto acaba en disco.** La contraseña de enlace que el orquestador
   entregó en memoria queda escrita en `ircd.conf` para toda la vida del
   contenedor, y en el volumen si `lib/` es un volumen.
2. **`sed` no respeta los valores.** Una contraseña con `/`, `&` o `\` produce
   un fichero corrupto o, peor, una sustitución distinta de la que se pedía.
   `envsubst` no tiene ese fallo pero tampoco tiene defectos ni obligatorias:
   sustituye todo `$COSA` que encuentre, incluido lo que no era una referencia.
3. **Una variable que falta se convierte en vacío, en silencio.** El servidor
   arranca con un `Connect` cuya contraseña es `""`. Nadie se entera hasta que
   el enlace no monta — o hasta que monta con quien no debía.
4. **Cada despliegue mantiene su plantilla y su entrypoint.** Nada de eso está
   en el proyecto ni lo prueba nadie, y una opción nueva de `ircd.conf` obliga a
   tocar los dos ficheros de cada despliegue.
5. **`/REHASH` no vuelve a mirar el entorno**: relee el fichero generado. Para
   que el rehash vea un valor nuevo hay que regenerar el fichero desde fuera,
   que es justo lo que el entrypoint hace una sola vez, al arrancar.

## 3. Diseño

Dos mitades, en un módulo nuevo (`ircd/ircd_env.c`) que no depende de nada del
servidor salvo el asignador y el log.

### 3.1 Accesores tipados

Para el código C que quiere *una* variable:

```c
const char* env_get(const char* name);
const char* env_str(const char* name, const char* def);
int         env_int(const char* name, int def, int min, int max);
int         env_bool(const char* name, int def);
```

`env_get()` es `getenv()` y nada más. Los otros tres tratan **vacío como no
puesto** — `FOO=` en un compose es la manera normal de decir "no lo pongas" —,
devuelven el defecto cuando el valor no sirve y dejan un aviso en `LS_CONFIG`
diciendo qué variable era. `env_bool()` acepta `1/true/yes/on/enabled` y
`0/false/no/off/disabled`, en cualquier caja.

### 3.2 Expansión de placeholders

Para la configuración:

```c
int   env_has_ref(const char* src);
char* env_expand(const char* src, unsigned int flags, struct EnvError* err);
int   env_expand_buf(char* dst, size_t size, const char* src,
                     unsigned int flags, struct EnvError* err);
int   env_expand_number(const char* src, unsigned int flags, int* out,
                        struct EnvError* err);
```

Las formas son las del shell POSIX, y sólo estas:

| Forma | Resultado |
|---|---|
| `${NAME}` | El valor. Sin valor es **error** (salvo `ENV_LENIENT`). |
| `${NAME:-palabra}` | El valor si está puesto y no vacío; si no, `palabra`. |
| `${NAME-palabra}` | El valor si está puesto (aunque sea vacío); si no, `palabra`. |
| `${NAME:+palabra}` | `palabra` si está puesto y no vacío; si no, nada. |
| `${NAME+palabra}` | `palabra` si está puesto; si no, nada. |
| `${NAME:?palabra}` | El valor si está puesto y no vacío; si no, falla con `palabra` como mensaje. |
| `${NAME?palabra}` | El valor si está puesto; si no, falla con `palabra`. |

`$$` es un `$` literal, y es como se escribe un `${` literal: `$${`. Un `$` que
no abre una referencia es un `$` corriente, así que una contraseña con dólares
sigue siendo la misma contraseña.

La `palabra` puede llevar referencias dentro (`${A:-${B:-none}}`), hasta
`ENV_MAX_DEPTH` niveles. **El valor de una variable no.** Lo que sale del
entorno se copia tal cual y no se vuelve a mirar: `FOO='${BAR}'` produce
literalmente `${BAR}`, nunca el valor de `BAR`. Esa es la regla que impide que
una variable — que puede venir de un secreto, de un `envFrom` o de quien lanzó
el contenedor — construya referencias a otras.

### 3.3 Dónde se expande

En el **lexer** (`ircd/ircd_lexer.c`), en dos sitios y ninguno más:

- **Dentro de una cadena entre comillas**, después de que el lexer ya la haya
  reconocido como cadena. El valor entra en el `QSTRING` que ya iba a producirse:
  no puede abrir un bloque, ni cerrar una sentencia, ni convertirse en una
  palabra clave, contenga lo que contenga. Es la propiedad de seguridad que
  hace que esto sea aceptable con valores que no controla el administrador.
- **Sin comillas, donde la gramática quiere un número** (`numeric = ${N};`,
  `port = ${PORT};`). Ahí la referencia se expande y el resultado tiene que ser
  un número decimal; cualquier otra cosa es un error de configuración. Es
  `env_expand_number()`, y existe precisamente para que un valor no pueda
  convertirse en otro token.

Como la expansión ocurre en el lexer, vale en todo el fichero sin tocar la
gramática: `Features { "HUB" = "${IRCD_HUB:-no}"; };` o
`include "${IRCD_CONF_DIR:-/opt/ircu/lib}/opers.conf";` funcionan solos.

### 3.4 Estricto por defecto

`${NAME}` sin valor **falla**. La alternativa (expandir a vacío, como
`envsubst`) es el problema 3 del §2: un servidor en pie con una contraseña
vacía. Quien quiera un valor opcional lo dice con `:-`, y quien quiera exigirlo
con un mensaje propio lo dice con `:?`.

Un fallo al expandir se reporta por `yyerror()`, con lo que queda en el log de
`LS_CONFIG`, en el snomask de los opers y en `stderr` durante el arranque, y
pone `conf_error`: `init_conf()` se niega a arrancar el servidor. En un
`/REHASH` el bloque afectado se descarta — que es el comportamiento que ya
tiene cualquier error de sintaxis, y es preferible a aplicar un bloque con
huecos.

### 3.5 Los diagnósticos no llevan valores

Ninguna función de este módulo imprime el valor de una variable: los mensajes
nombran la variable y paran ahí. El entorno es de donde vienen las contraseñas
de enlace y los hashes de oper, y los errores de configuración se difunden a
todos los opers conectados.

```
Config file parse error line /opt/ircu/lib/ircd.conf:41: environment variable
IRCD_LINK_PASS: ponga la contraseña de enlace
```

`env_expand_number()` es la única que cita texto — el texto de la *referencia*,
`"${IRCD_NUMERIC}"`, no el valor.

## 4. Ejemplo completo

`ircd.conf`, una sola vez, en la imagen:

```
General {
        name        = "${IRCD_NAME}";
        numeric     = ${IRCD_NUMERIC};
        description = "${IRCD_DESCRIPTION:-${IRCD_NAME} (${IRCD_NET:-undernet})}";
};

Connect {
        name     = "hub.example.net";
        host     = "${IRCD_HUB_HOST:-10.55.0.10}";
        password = "${IRCD_LINK_PASS:?ponga la contraseña de enlace}";
        port     = ${IRCD_HUB_PORT:-4400};
        class    = "Server";
};

Features {
        "HUB" = "${IRCD_HUB:-FALSE}";
};
```

y el entrypoint entero:

```sh
exec /opt/ircu/bin/ircd -f /opt/ircu/lib/ircd.conf -n
```

Sin plantilla, sin `sed`, sin secreto en disco, y con el arranque abortado si
falta la contraseña en vez de un enlace abierto con contraseña vacía.

## 5. Ciclo de vida

El entorno de un proceso Unix se fija al ejecutarlo: nadie lo cambia desde
fuera. Un `/REHASH` vuelve a leer `ircd.conf` y vuelve a expandir, así que ve
exactamente los mismos valores que al arrancar — un valor nuevo exige reiniciar
el proceso, que es también lo que exige cualquier orquestador para cambiar el
entorno de un contenedor. Esto no es una regresión respecto al método legacy:
allí el rehash tampoco veía nada nuevo, releía el fichero ya generado.

El entorno sobrevive al `chroot` (§`doc/readme.chroot`), de modo que un secreto
pasado por variable sigue disponible donde un fichero fuera del chroot ya no lo
estaría.

## 6. Módulos

No hace falta API nueva ni tocar la ABI. Un módulo se enlaza contra los
símbolos del ejecutable, así que `#include "ircd_env.h"` y llamar a
`env_str()` o `env_expand()` funciona tal cual — no hay estado por módulo que
registrar ni que revertir al descargar, que es lo que justifica un
`module_*()` en los demás subsistemas.

## 7. Implementación

| Fichero | Qué |
|---|---|
| `include/ircd_env.h` | API, sintaxis y reglas, documentadas con Doxygen. |
| `ircd/ircd_env.c` | Accesores y expansor. ~500 líneas, sin dependencias del core más allá de `ircd_alloc.h` e `ircd_log.h`. |
| `ircd/ircd_lexer.c` | `lexer_qstring()` para `QSTRING`; rama nueva para `${...}` sin comillas → `NUMBER`. |
| `ircd/test/ircd_env_t.c` | Test unitario (CTest), 12 grupos de casos. |
| `doc/readme.env` | Documentación para quien administra el servidor. |

Límites, todos en el header: `ENV_NAME_MAX` 128, `ENV_EXPAND_MAX` 8192,
`ENV_MAX_DEPTH` 8. El buffer del lexer no cambia: la expansión ocurre sobre el
token ya reconocido, en memoria propia.

## 8. Pruebas

`ctest -R ircd_env_t` cubre las siete formas, vacío contra no puesto, el
anidamiento y su límite, que los valores no se re-escanean, `$$`, cada error de
sintaxis, los límites de longitud, `env_expand_buf()`, `env_expand_number()` y
los accesores tipados.

Para la integración basta `ircd -k` con un `ircd.conf` que use referencias, con
y sin las variables puestas. Un test de `tests/` que arranque un contenedor con
`environment:` en vez de un `.conf` horneado es trabajo futuro razonable; no lo
incluye esta propuesta porque exige una topología nueva en el compose.

## 9. Alternativas descartadas

- **Seguir plantillando fuera.** Es el §2.
- **Expandir en el parser, sobre cada valor ya parseado.** Habría que tocar
  cada regla de la gramática y cada sitio que guarda una cadena; el lexer lo
  hace una vez, para todo el fichero, y además puede decidir que el resultado
  de una referencia sin comillas tiene que ser un número.
- **Re-lexar el texto expandido.** Permitiría `${IRCD_EXTRA_BLOCKS}` con
  bloques enteros dentro, y con ello que una variable de entorno escribiera un
  `Operator` nuevo. No.
- **Expandir también el valor de las variables.** Bucles, y una variable
  pudiendo alcanzar a otra que el administrador no pensaba exponer.
- **Un bloque `Environment { prefix = "IRCD_"; };`** que limite qué variables
  puede leer la configuración. El proceso ya corre con ese entorno: quien lo
  fija es quien lanza el servidor, y no gana nada protegiéndose de sí mismo.
- **Variables propias del fichero de configuración** (`define X = ...;`). Es
  otro problema — y si algún día se quiere, `${...}` ya es la sintaxis: bastaría
  con buscar primero en una tabla propia y luego en el entorno.

## 10. Trabajo futuro

- `IRCD_CONF` / `IRCD_DPATH` como defectos de `-f` y `-d`, con el mismo API,
  para que el contenedor no necesite argumentos.
- Una topología de `tests/` que configure un servidor sólo con `environment:`.
- `Features` alimentadas directamente desde el entorno (`FEAT_*` con un prefijo
  convenido), si aparece la necesidad; hoy se cubre con `"${...}"` en el bloque
  `Features`.
