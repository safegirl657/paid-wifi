PAID WIFI (ESP8266) - voucher + timer

COMPILE (kono IDE install lagbe na):
1. src/main.cpp er upore UP_SSID / UP_PASS (tomar router er WiFi), ADMIN_PASS, AP_SSID, PRICE_INFO change koro.
2. Ei pura folder ta GitHub e notun repo te upload koro.
3. GitHub -> Actions tab -> "Build firmware" -> run shesh hole niche Artifacts theke "firmware" download koro (firmware.bin).

FLASH:
1. https://esptool.spacehuhn.com/ (Chrome/Edge)
2. Connect -> COM port select -> firmware.bin, address 0x0 -> Program

USE:
1. ESP8266 power dao. "Paid WiFi" name e WiFi ashbe.
2. Phone connect korle login page ashbe (na ashle browser e http://10.10.10.1 kholo).
3. Admin: http://10.10.10.1/admin (user: admin, pass: ADMIN_PASS).
4. Customer code dile oi time er jonno internet cholbe. Time shesh hole abar block.

NOTE: board 4MB na hole (jemon ESP-01 1MB) platformio.ini te board = esp01_1m koro.

BKASH FLOW:
- Customer package select kore BKASH_NUMBER e Send Money kore, nijer bKash number + TrxID dey.
- Admin panel (/admin) e "bKash requests" e pending request dekhay (package, sender number, TrxID).
  Tumi bKash app e TrxID milaye "Approve" chaple oi device er net chalu hoy. "Reject" chaple bondho.
- Customer er screen auto update hoy (4 sec por por check kore).
- Package er dam/time main.cpp er PKG list e change koro.

REDEEM CODE:
- Admin panel e "Make redeem codes" theke code banao. Customer "Redeem code" box e code dile net chalu.
