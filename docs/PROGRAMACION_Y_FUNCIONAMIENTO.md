# Programación y funcionamiento (revisión 2026-11-16)

## Editor de bloques

Desactivar las reglas y detener la cabina antes de editar. Cada bloque conserva
su entrada GPIO, nivel activo, salida, valor y reposo; ahora también tiene:

- Condición: GPIO, botón SUBIR, botón BAJAR, botón CABINA, en piso, subiendo,
  bajando, puertas abiertas o llamada pendiente.
- Piso: cualquier piso o uno específico. Para botones es el piso de la
  botonera; para GPIO/dirección/puertas restringe a la posición confirmada.
- Acción: escribir GPIO o registrar una llamada interior/exterior con destino.
  El destino es obligatorio para llamadas; no se usa la salida GPIO en ese caso.

Ejemplos (elegir pines realmente libres en el cableado):

| SI | Piso | Nivel | ENTONCES | Reposo |
|---|---|---|---|---|
| Botón SUBIR | 2 | 1 | GPIO 25 = 1 | 0 |
| Botón CABINA | 3 | 1 | GPIO 27 = 1 | 0 |
| GPIO 28 | cualquiera | 1 | Pedir cabina, destino 2 | no aplica |
| Subiendo | cualquiera | 1 | GPIO 49 = 1 | 0 |
| Bajando | cualquiera | 1 | GPIO 50 = 1 | 0 |

Los botones virtuales producen pulsos de **300 ms**, incluso si la cabina ya
está en ese piso o la llamada está repetida. No se asigna automáticamente un
GPIO: se elige la salida mediante un bloque. Para mantener una señal hasta que
se atienda el pedido, usar «Llamada pendiente». Las acciones de llamada se
procesan al pasar de condición falsa a verdadera, evitando repetirlas cada 10 ms.
Los GPIO físicos se filtran 30 ms. Los sensores de piso se muestrean cada 100 ms;
la confirmación requiere muestras estables y puede tardar unos 200 ms.

Varias condiciones de salida se combinan mediante OR: basta una condición
activa para escribir el valor activo. Todas deben compartir valor activo y
reposo. No se permiten pines del panel ni salidas sobre sensores de piso.
El esquema NVS pasa a `rules_v2`; los bloques `rules_v1` se leen como reglas
GPIO y conservan sus notas. Un esquema desconocido se rechaza.

## Recorrido colectivo y pantalla

Se guardan por separado las llamadas interiores, exteriores de subida y de
bajada, con hasta 16 pisos y 32 bloques en total. Durante el recorrido se evalúa
cada piso confirmado, incorporando pedidos nuevos sin esperar al destino inicial.
Las llamadas interiores y las exteriores del mismo sentido provocan parada.
Las exteriores del sentido contrario se conservan para el regreso, salvo en
el extremo del recorrido pendiente, donde puede invertirse el sentido.

No se admiten BAJAR en el piso inferior ni SUBIR en el superior. Un pedido en
el piso actual detenido deja intacta la cuenta de una salida programada. Se conserva el
sentido entre paradas para continuar la recogida. La vista **Técnica → Ver pisos**
es desplazable y muestra todos los pisos, sensor asignado, cabina confirmada y
llamadas C/S/B. La botonera interior también distingue posición y pedidos.

La posición solo procede de sensores. No hay avance por temporizador ni opción
de modo demo; la clave/los campos antiguos se conservan inactivos por compatibilidad.
Sin un sensor confirmado no se inicia una ruta. Varios sensores simultáneos
registran un fallo retenido, detienen la lógica de movimiento y ponen las reglas
en reposo. Corregir las señales, desactivar bloques y reaplicar configuración para
reinicializar. Entre pisos no se inventa una posición.

La pantalla arranca con las puertas dibujadas abiertas. La primera solicitud
desde una parada inicia una espera de 3 s. Al llegar a una parada solicitada,
se abren las puertas y, si quedan solicitudes, inicia otra espera de 3 s.
La animación de cierre comienza durante los últimos 700 ms de cada espera.
En la última parada las puertas permanecen abiertas hasta la próxima solicitud.

## Qué falta respecto a un ascensor real

El editor sigue siendo un motor de reglas acotado, no un PLC general: faltan
expresiones AND/OR arbitrarias, temporizadores configurables, memorias y secuencias
programables. El recorrido colectivo continúa siendo parte del controlador C.

La variable de puertas sigue siendo una **orden lógica con temporizador de 3 s**;
no confirma apertura, cierre ni enclavamiento físicos. Para convertirlo en control
de una instalación hacen falta señales y decisiones de hardware concretas:

| Función pendiente | Información / implementación necesaria |
|---|---|
| Puertas | Contactos de cierre y enclavamiento, fotocélula, abrir/cerrar, obstrucción y tiempos máximos |
| Movimiento | Contactores mutuamente enclavados, freno, realimentación y velocidad / nivelación |
| Límites | Finales superior/inferior independientes, coherencia y orden de sensores |
| Averías | Tiempo máximo entre pisos, fallo de sensor, fallo de contactor y recuperación supervisada |
| Carga y emergencia | Sobrecarga, cadena de seguridad, parada de emergencia, alarma y rescate |
| Servicio | Inspección, bomberos, prioridad, aparcamiento y tratamiento de corte de energía |
| E/S | Solo hay 11 GPIO J7; 16 sensores más botones y actuadores requieren ampliar E/S |

El reposo de cada salida es configurable; debe corresponder al circuito cableado.
Las reglas no reemplazan los enclavamientos eléctricos independientes. No se ha
verificado accionamiento físico, distancia de frenado ni comportamiento de puertas.

## Verificación

`bash tests/host/run.sh` compila el controlador y el motor reales con GPIO/reloj
de prueba y sanitizadores de memoria/comportamiento indefinido. Comprueba paradas
intermedias de ambas procedencias, llamadas contrarias retenidas incluso en un
piso servido por llamada interior, pulsos, GPIO físico → llamada, OR de salidas,
reposo, conflicto de sensores y límites de llamadas exteriores.

No valida LVGL en pantalla, NVS en la placa ni el firmware completo. Requiere
compilar con ESP-IDF y probar cableado y pantalla en la CrowPanel correspondiente.

## Configuración de referencia de la CrowPanel de tres pisos

| Boton de cabina | GPIO de salida | Entrada del PLC |
| --- | --- | --- |
| P01 | 52 | X000 |
| P02 | 51 | X001 |
| P03 | 50 | X002 |

Cada bloque usa **Programacion tipo Boton**, fuente **CABINA**, filtro por el
piso de destino y condicion **Al pulsar**. La salida se guarda con activo 1
y reposo 0. El pulso dura unos 300 ms. Los sensores de piso 1, 2 y 3 usan
GPIO 2, 3 y 4, respectivamente. Los bloques se configuraron en NVS en la
placa de prueba. En otra placa se pueden reproducir con esta tabla.

En Exterior e Interior, el sentido observado se obtiene comparando los dos
ultimos pisos distintos detectados. Hasta que hay una transicion verificable
se muestra **Sentido sin confirmar**. La prueba fisica de recorrido queda
pendiente.

## Verificación en la pantalla de la práctica

### Configuración de los siete bloques

Las cuatro fotos del 23 de noviembre de 2026 muestran el interruptor de
programación por bloques encendido y el indicador **BLOQUES ACTIVOS 7/32**.
Los siete bloques usan **Programación tipo Botón**, condición **Al pulsar**
y acción **Salida GPIO** con **ESCRIBIR 1**.

| Bloque | Botón | Filtro | Salida GPIO | Escribir | Reposo |
| ---: | --- | --- | ---: | ---: | ---: |
| 1 | CABINA | Piso 1 | 52 | 1 | 0 |
| 2 | CABINA | Piso 2 | 51 | 1 | 0¹ |
| 3 | CABINA | Piso 3 | 50 | 1 | 0 |
| 4 | SUBIR | Piso 1 | 52 | 1 | 0¹ |
| 5 | BAJAR | Piso 3 | 50 | 1 | 0 |
| 6 | SUBIR | Piso 2 | 51 | 1 | 0 |
| 7 | BAJAR | Piso 2 | 51 | 1 | 0¹ |

¹ El campo **REPOSO** queda fuera de la foto en los bloques 2, 4 y 7.
Se deduce el valor 0 porque el editor muestra los siete bloques activos y el
motor exige el mismo reposo en todos los bloques que comparten un GPIO.
El reposo 0 se ve directamente en el bloque 1 para GPIO 52 y en el 6 para
GPIO 51.

En una CrowPanel sin configuración guardada, el firmware arranca con tres
pisos y estos siete bloques activos. Los bloques guardados en NVS, incluso
una lista vacía, tienen prioridad. Si ya hay una configuración de tres pisos
pero falta la lista de bloques, se precargan los siete y se respeta el
estado guardado del interruptor. Los GPIO y niveles de los sensores de piso
se configuran en Técnica.

La botonera de cabina y las llamadas exteriores comparten una salida por
piso. En el piso 1, **CABINA** o **SUBIR** activan GPIO 52. En el piso 2,
**CABINA**, **SUBIR** o **BAJAR** activan GPIO 51. En el piso 3, **CABINA**
o **BAJAR** activan GPIO 50. El motor combina las condiciones de cada salida
por OR. El firmware configura un pulso de 300 ms para los botones. Con el
cableado de referencia anterior, el PLC recibe una solicitud de piso en
X000, X001 o X002. La solicitud del piso 2 llega por X001 tanto al pulsar
**SUBIR** como al pulsar **BAJAR**.

Las fotos confirman la configuración visible. La comprobación de los pulsos
en las entradas del PLC y del movimiento físico requiere una prueba en el
equipo.

### Pruebas de la actualización

| Prueba | Cambio incorporado en GitHub | Resultado en la CrowPanel |
|---|---|---|
| Animación de la puerta de la cabina | `bc8c4a2` | Pendiente de observar la apertura al llegar y el cierre antes de la siguiente salida |
| Giro de pantalla y táctil | `178fbfe` | Pendiente de probar vertical → horizontal → vertical y comprobar que los toques coincidan en ambas orientaciones |

Registrar cada hallazgo con la pantalla y orientación, los pasos para
reproducirlo y el resultado observado.
