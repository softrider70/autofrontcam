/*
 * voltage.h - Spannungsmessung (Fahrzeugbatterie) fuer Autofrontcam
 *
 * Misst die Bordspannung ueber einen Spannungsteiler (100k/22k) an GPIO35
 * (ADC1_CH7). Bietet den Betriebsmodus "Geregelt" (Sleep/Wake-up) bzw.
 * "Dauerbetrieb" (immer aktiv) sowie den einstellbaren Schwellwert.
 */

#ifndef VOLTAGE_H
#define VOLTAGE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ADC initialisieren + Modus/Grenzwert aus NVS laden */
esp_err_t voltage_init(void);

/* Aktuelle Batteriespannung in Volt lesen (Mittelwert aus 16 Samples) */
float voltage_read_batt(void);

/* Zuletzt gemessene Spannung (ohne neue Messung) */
float voltage_get_last(void);

/* Schwellwert in Volt (Werkseinstellung 12,8V) */
float voltage_get_threshold(void);
void voltage_set_threshold(float volts);

/* Betriebsmodus: true = Geregelt (Sleep aktiv), false = Dauerbetrieb */
bool voltage_is_regulated(void);
void voltage_set_mode(bool regulated);

/* Soll in den Sleep gewechselt werden?
 * Nur im Modus "Geregelt" und wenn die Spannung unter dem Schwellwert liegt. */
bool voltage_sleep_required(void);

#ifdef __cplusplus
}
#endif

#endif /* VOLTAGE_H */
