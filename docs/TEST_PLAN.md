# Manual test plan

Everything below can be done from the dashboard with the built-in simulator
(**Test bench** tab), no hardware needed. With the real ESP32, do the same
actions physically (tap a card, drive the model car past the sensors, pull the
Wi-Fi) and watch the same results in the dashboard.

Start: `cd backend && npm install && npm start`, open http://localhost:8080, sign in with `admin`.

Demo cards created on first start:

| Holder | UID | Status |
| --- | --- | --- |
| Jubayer Alam | `A1:B2:C3:D4` | active |
| Sara Lindqvist | `11:22:33:44` | active |
| Omar Hassan | `DE:AD:BE:EF` | active |
| Guest card 1 | `0A:1B:2C:3D` | blocked |

## 1. Sign in

| Step | Expected |
| --- | --- |
| Open the dashboard, type a wrong password | "Wrong password." |
| Sign in with the right password | Dashboard opens. Header shows "Gate controller online (simulator)" and "MQTT broker connected". |
| Click **Sign out** | Back to the sign-in page. API calls are refused until you sign in again. |

## 2. Normal entry and exit (RFID + sensors + barrier + LCD + buzzer)

| Step | Expected |
| --- | --- |
| Test bench: choose *Jubayer Alam*, click **Drive a car in** | Entry LCD: "Scan Your Card" → "Welcome Jubayer A / Gate Opening..." → "Vehicle Detected / Please Proceed" → "Free: 9/10". Barrier in the top strip lifts and closes. Buzzer pill shows one beep. Free count drops by one and one bay fills. |
| Activity tab | New row "Vehicle entered", Jubayer Alam, entry gate, "1 occupied after". |
| Cards tab | Jubayer Alam shows "Inside", visits 1. |
| Test bench: **Drive a car out** with the same card | Exit LCD says "Goodbye ...", free count goes up, Activity shows "Vehicle left", card shows "Outside". |

## 3. Safe closing (the barrier never closes on a car)

| Step | Expected |
| --- | --- |
| Entry gate: click **Tap card on reader** with an active card | Barrier opens. |
| Tick **Car under / just past the barrier** and wait more than 15 s | Barrier stays open as long as the box is ticked. LCD: "Vehicle Detected". |
| Untick it | Barrier closes, counter goes up by one. |
| Tap an active card and do nothing for 15 s | Barrier closes by itself. Activity shows "Gate opened, no vehicle passed". Counter unchanged. |

## 4. Denied access

| Step | Expected |
| --- | --- |
| Choose *Unregistered card*, **Tap card on reader** | LCD "INVALID CARD / Access Denied!", buzzer three beeps, barrier stays closed. Activity: "Access denied" + "Card is not registered" + a **Register this card** button. |
| Choose *Guest card 1 – blocked*, tap | Denied, Activity note "Card is blocked". |
| Type any UID in the free-text box (e.g. `12 34 56 78`), tap | Denied as not registered. |

## 5. Card management reaches the device

| Step | Expected |
| --- | --- |
| Activity: **Register this card** on the unknown card, enter a name, save | Toast "Card added and sent to the gate controller". Test bench → Flash memory: card-list version went up by one and the card count by one. |
| Tap that card again | Accepted now. |
| Cards: **Block** it, tap again | Denied, "Card is blocked". |
| Cards: **Unblock**, **Edit** the name, **Delete** | Each change raises the card-list version on the device (Settings → "Card list on device ... (up to date)"). |
| Cards: **Add card** with UID `zz` | Error explaining the UID format. Adding an existing UID says it is already registered. |

## 6. Lot full

| Step | Expected |
| --- | --- |
| Settings: set Total spaces 2, Occupied 2, **Save changes** | Top strip: "0 free of 2", entry LCD "Parking FULL". |
| Drive a car in | Denied, note "Parking lot is full". Barrier stays closed. |
| Drive a car out | Allowed. Counter 1 of 2. |
| Settings: back to 10 total | Strip shows 10 bays again. Both changes appear in Activity as admin actions. |

## 7. Remote control

| Step | Expected |
| --- | --- |
| Click **Open exit gate** in the top strip | Barrier lifts within a second, button is disabled while open, closes after 15 s (or after a car passes). Activity logs "Remote open of exit gate". |

## 8. Network outage: edge resilience and store & forward

| Step | Expected |
| --- | --- |
| Test bench: switch **Network** off | Header turns red: "Gate controller offline". Yellow banner explains. LCDs show `*` in the top-right corner. **Open gate** buttons are disabled. |
| Drive two cars in, tap the unregistered card once | Gates still work from the cached card list. Flash memory lists 3 events waiting. Activity shows nothing new and the free count in the top strip does not move. |
| Cards: add a new card while the device is offline | Saved on the server. Device still has the old list. |
| Switch **Network** on | Within ~2 s: header green, the 3 events appear in Activity with their real times and a "Buffered offline" tag, free count jumps to the right value, flash queue is empty, card-list version on device is up to date. |
| Statistics | "events delivered after an outage" shows 3. |

## 9. Power cut on the device

| Step | Expected |
| --- | --- |
| Switch **Power** off, then on | Counter and card list are the same as before (read from flash). Serial monitor shows the boot number going up. |
| Network off → drive a car in → Power off → Power on → Network on | The event buffered before the power cut is still delivered. |
| **Erase flash**, Power off/on | First-boot behaviour: LCD "System Init / Waiting sync..." until the card list arrives, counter restored from the backend. |

## 10. Backend restart

| Step | Expected |
| --- | --- |
| Stop the backend (Ctrl+C) and start it again | All cards, activity and settings are still there (SQLite file `backend/data/parking.db`). |

## 11. Statistics and export

| Step | Expected |
| --- | --- |
| Statistics tab, switch Today / Last 24 hours / Last 7 days | Counters and both charts follow. "Inside right now" matches the Cards tab. |
| Activity: filter "Denied", search a name, **Export CSV** | Table filters live. The CSV contains the filtered rows and opens in Excel. |
| Settings: **Delete activity log** | Activity and charts are empty, cards remain. |

## 12. Real hardware

1. Test bench: switch **Power** off (or start the backend with `SIMULATOR=off`).
2. `include/secrets.h`: Wi-Fi name/password and the IP shown under Settings → "ESP32 connects to".
3. Upload with PlatformIO, open the serial monitor (115200). Type `status`.
4. Header shows "Gate controller online (esp32-gate)". Repeat sections 2 to 9 with real cards. For section 8, switch off the Wi-Fi router or stop the backend.

Serial commands: `status`, `dump`, `count`, `clear`, `sync`, `cache`, `refresh`, `fsinfo`, `open entry`, `open exit`, `help`.

## Automated check

`cd backend && npm test` runs 13 end-to-end tests (real broker, database, API and simulator) covering sections 1 to 9 and 11.
