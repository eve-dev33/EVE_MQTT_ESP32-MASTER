# Flow schedulazioni POWER (APP ↔ MASTER ↔ SLAVE)

## Diagramma testuale

1. APP pubblica `progetto/EVE/POWER/relay/{ch}/schedule/set` con JSON array (max 10 regole).
2. MASTER valida payload (`at`, `state`, `days`) e controlla idempotenza.
3. MASTER salva in `pending` e invia packet ESP-NOW `type=14` (`PowerRelayRulesPacket`).
4. MASTER attende ACK `type=15` entro timeout configurabile (default 3000 ms).
5. Se timeout: retry invio ESP-NOW (almeno 1 retry).
6. Se ACK positivo (`ok=1`):
   - publish `.../schedule/slave/ack = OK`
   - publish retained `.../schedule = OK SCHEDULAZIONE`
   - publish retained `.../schedule/current = JSON`
   - persistenza schedule su NVS.
7. Se ACK negativo o timeout finale:
   - publish `.../schedule/slave/ack = ERROR`
   - log strutturato con relay/esito.
8. Quando SLAVE invia `type=16` (`PowerExecutedPacket`):
   - MASTER aggiorna stato relay
   - publish `.../executed = ON|OFF`
   - publish `.../state = ON|OFF`

## Note compatibilità

- Topic manuali invariati:
  - `progetto/EVE/POWER/relay/%d/set` (`ON|OFF|TOGGLE`)
  - `progetto/EVE/POWER/relay/%d/state` (`ON|OFF`)
- Protocollo POWER rispettato su packet `type=14/15/16`.
- A reconnect MQTT il MASTER ripubblica `schedule/current` retained caricando da persistenza locale.
