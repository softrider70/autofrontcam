/*
 * dns_server.h - DNS-Intercept fuer Autofrontcam
 *
 * Beantwortet ALLE DNS-Anfragen mit der AP-IP (10.1.1.1). Dadurch wird
 * captive.apple.com, connectivitycheck.gstatic.com usw. auf den ESP umgeleitet,
 * und iOS/Android erkennen das Netzwerk als Captive Portal und oeffnen den
 * Browser automatisch auf der Web-UI.
 */

#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void dns_server_start(void);
void dns_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* DNS_SERVER_H */
