/*
 * sleep.h - Sleep-/Wake-up-Steuerung fuer Autofrontcam
 *
 * Im Modus "Geregelt" geht der ESP in den Deep-Sleep, sobald die Spannung
 * unter den Schwellwert faellt (Fahrzeug abgestellt). Wake-up erfolgt per
 * RTC-Timer: periodisch kurz aufwachen, Spannung messen, ggf. weiter schlafen
 * oder (bei Spannungsanstieg) normal hochfahren (AP + Kamera + Stream).
 */

#ifndef SLEEP_H
#define SLEEP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Early-Check direkt nach voltage_init(): Wenn "Geregelt" und Spannung unter
 * dem Grenzwert -> sofort in den Deep-Sleep (kehrt nicht zurueck). */
void sleep_check_early(void);

/* Spannungs-Monitor-Task starten (Deep-Sleep bei Unterschreitung waehrend
 * des Betriebs). Im Modus "Dauerbetrieb" wird keine Task erstellt. */
void sleep_start_monitor(void);

#ifdef __cplusplus
}
#endif

#endif /* SLEEP_H */
