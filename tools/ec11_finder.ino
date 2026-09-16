// EC11 pin finder - ESP32-C3 SuperMini
//
// Watches EVERY usable GPIO and reports any that change when you turn or press the encoder.
// It assumes nothing about which pins you soldered to, so it also finds mis-wiring.
//
// REQUIRED:  Tools > USB CDC On Boot > "Enabled"
//            Without it, Serial is routed to UART0, which IS GPIO20/21 - the pins your
//            encoder is on. You would see nothing and the encoder would be dead. That is
//            the prime suspect for "nothing works".
//
// It alternates the input mode every 8 seconds:
//   PULLUP   pins idle HIGH, go LOW when a contact closes to GND   <- normal wiring
//   PULLDOWN pins idle LOW,  go HIGH when a contact closes to 3V3  <- common on 3V3 by mistake
// If the encoder only responds in PULLDOWN, its common pins are on 3V3 instead of GND.

const uint8_t PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21};
const uint8_t N = sizeof(PINS) / sizeof(PINS[0]);

uint8_t  last[N];
uint32_t hits[N];
bool     pulldownMode = false;
uint32_t modeSince    = 0;

void applyMode() {
  for (uint8_t i = 0; i < N; i++)
    pinMode(PINS[i], pulldownMode ? INPUT_PULLDOWN : INPUT_PULLUP);
  delay(20);
  for (uint8_t i = 0; i < N; i++) last[i] = digitalRead(PINS[i]);

  Serial.printf("\n---- mode: %s ----\nidle: ", pulldownMode ? "INPUT_PULLDOWN" : "INPUT_PULLUP");
  for (uint8_t i = 0; i < N; i++) Serial.printf("%d=%d ", PINS[i], last[i]);
  Serial.println("\n");
  modeSince = millis();
}

void setup() {
  Serial.begin(115200);
  delay(3000);                       // let the USB serial port enumerate before we talk
  Serial.println("\n\n=== EC11 pin finder ===");
  Serial.println("Turn the shaft SLOWLY one detent at a time, then press it.");
  Serial.println("Anything that changes gets printed with its pin number.\n");
  for (uint8_t i = 0; i < N; i++) hits[i] = 0;
  applyMode();
}

void loop() {
  for (uint8_t i = 0; i < N; i++) {
    uint8_t v = digitalRead(PINS[i]);
    if (v != last[i]) {
      last[i] = v;
      hits[i]++;
      Serial.printf("GPIO%-2d -> %d      (%lu changes)\n", PINS[i], v, hits[i]);
    }
  }

  static uint32_t lastReport = 0;
  if (millis() - lastReport > 4000) {
    lastReport = millis();
    Serial.print("   totals: ");
    bool any = false;
    for (uint8_t i = 0; i < N; i++)
      if (hits[i]) { Serial.printf("GPIO%d=%lu  ", PINS[i], hits[i]); any = true; }
    Serial.println(any ? "" : "NOTHING HAS CHANGED ON ANY PIN YET");
  }

  if (millis() - modeSince > 8000) {
    pulldownMode = !pulldownMode;
    applyMode();
  }
}

// ---------------------------------------------------------------------------
// READING THE RESULT
//
// Two pins change together as you rotate, out of phase   -> those are A and B. Good.
// One pin changes only when you press                    -> that is the switch.
// A pin sits at 0 permanently in PULLUP mode             -> it is shorted to GND. Most
//                                                           likely the encoder's COMMON
//                                                           (middle pin of the 3) is
//                                                           soldered to a GPIO instead
//                                                           of to ground.
// Pins respond in PULLDOWN but not PULLUP                -> commons are wired to 3V3.
//                                                           Move them to GND.
// Nothing changes in either mode                         -> the commons are not connected
//                                                           to anything. On the 3-pin side
//                                                           the MIDDLE pin is common; the
//                                                           2-pin switch side has its own
//                                                           common. BOTH need grounding.
//
// Before blaming the firmware, confirm the part with a multimeter on continuity:
//   middle pin to each outer pin  -> makes and breaks as you click through detents
//   the two switch pins           -> open, closing when pressed
// ---------------------------------------------------------------------------
