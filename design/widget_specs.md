# Console Plate — widget specifikáció

Minden méret logikai pixel 1× skálán. Színnevek a `console_plate.h` / `tokens.json` tokenjei.
Sűrűség: `control_h` = 40 (comfort) vagy 32 (compact); `row_h` ugyanígy. Minden más méret **sűrűségtől független**.

Rajzolási sorrend minden widgetnél: (1) árnyék, (2) kitöltés, (3) 1px keret, (4) belső grafika (pipa, pont, hüvely, fill), (5) szöveg, (6) fókuszgyűrű **legfelül**.

---

## 0. Általános szabályok

- **Keret:** 1px `line`, a widget külső élén, radius `4` (tálcán belüli elem: `3`).
- **Felületrétegek:** `bg` (asztal) → `surface` (ablaktörzs) → `sunken` (input/lista) / `raised` (címsor, menü, dialógus, toast). Egy widget soha nem használ kettőnél több réteget.
- **Állapotsor** (minden kattintható elem): `alap` → `hover` → `pressed` → `focus` → `disabled`.
  - hover: `hover` kitöltés (accent-kitöltésű elemnél világosítás ~+8%)
  - pressed: `press` kitöltés (accent-kitöltésűnél ~−12% világosítás)
  - focus: 2px `focus` gyűrű, 1px offset, a keret **kívül**, a kitöltés nem változik
  - disabled: a teljes widget 45% alfával kompozitálva, kurzor nem változik
- **Fókusz mindig látszik**, ha billentyűzetről érkezett a fókusz. Egérrel is látszódhat — a hobbi-OS-ben az egyszerűbb: mindig rajzold.
- **Szöveg vertikálisan középen**, a baseline a widget középvonala + fontméret * 0.36 lefelé.
- **Találati terület:** desktop buildben = widget mérete; touch buildben minden kattintható elem legalább 44×44 (a rajz mérete változatlan, a hitbox nő).

---

## 1. Gomb

| rész | érték |
|---|---|
| magasság | `control_h` |
| vízszintes belső margó | `control_pad_x` (18 / 14) |
| radius | 4, keret 1px |
| szöveg | body font, primary-nál weight 500 |

Változatok:
- **primary** — kitöltés `accent`, keret `accent`, szöveg `on_accent`
- **secondary** — kitöltés `raised`, keret `line`, szöveg `text`
- **ghost** — kitöltés átlátszó, keret átlátszó, szöveg `accent`; belső margó 12
- **ikon** — `control_h` × `control_h`, secondary színek, glyph középen
- **gombcsoport** — közös 1px `line` keret, radius 4 csak a két végén; elválasztó 1px `line`

Állapotok pontosan:
- primary hover `brightness 1.08`, pressed `brightness 0.88`
- secondary hover `hover`, pressed `press`
- ghost hover `hover`, pressed `press`

---

## 2. Checkbox

- Doboz 20×20, radius 4, keret 1px `line`.
- **ki:** kitöltés `sunken`.
- **be:** kitöltés `accent`, benne pipa `on_accent` színnel — 2px vonal, két szakasz: (5,11)→(8,14)→(15,6) a 20×20 dobozon belül.
- **részleges (indeterminate):** kitöltés `accent`, benne 10×2 vízszintes sáv középen, `on_accent`.
- Címke 11px réssel jobbra, teljes sor magassága `control_h`, a teljes sor kattintható.
- Fókusz: a **sor** körül gyűrű (nem csak a dobozon), 4px radius.

## 3. Radio

- Kör 20 átmérő, keret 1px `line`, kitöltés `sunken`.
- **be:** a 20-as körön belül 4px széles gyűrű marad a szülő felület színe, a maradék közép `accent` — gyakorlatban: rajzolj `accent` korongot d=10 a közepére.
- Csoporton belül ↑↓ / ←→ léptet, a csoport egyetlen tab-stop.

## 4. Switch

- Sáv 48×26, radius 13, keret 1px `line`.
- **ki:** kitöltés `switch_off`; **be:** kitöltés `accent`.
- Hüvely (knob) 18×18 kör, `knob` szín, `shadow_sm`, 3px inset; x = 3 (ki) → 25 (be), animáció 120ms lineáris.
- Space / Enter kapcsol. A sor teljes szélessége kattintható; beállítás-listában a switch jobbra igazítva.

## 5. Szegmentált / tabok

- Tálca: kitöltés `tray`, keret 1px `line`, radius 4, belső margó 3.
- Elem: magasság 30–32, belső margó 16, radius 3.
- **aktív:** kitöltés `raised` (light: `#ffffff`, dark: `#416180`), szöveg `text`.
- **inaktív:** átlátszó, szöveg `muted`; hover `hover`.
- ←→ léptet, a csoport egy tab-stop.

## 6. Szövegbevitel

- Magasság `control_h`, belső margó 12, radius 4, keret 1px `line`, kitöltés `sunken`.
- Szöveg: útvonal/érték **mono**, szabad szöveg body font. Kurzor 1px `accent`, villogás 530ms.
- hover: keret `accent`. focus: keret `accent` + 2px `focus` gyűrű **offset 0** (a kereten).
- **hibás:** keret `accent`, plusz 3px bal oldali `accent` sáv a kereten belül; a hibaüzenet 12px `accent` szöveg alatta.
- **letiltva:** 45% alfa, kitöltés `sunken`.
- Címke fölötte: label font (Barlow Condensed 11px, UPPERCASE, ls 0.14em), `muted`, 6px rés.
- Segédszöveg alatta: 12px, `muted`.

## 7. Dropdown (select)

- Zárt állapot = szövegbevitel-geometria, jobb oldalon 10px `accent` háromszög (lefelé), 12px a jobb szélről.
- Nyitott lista: `raised` kitöltés, 1px `line`, radius 4, `shadow_lg`, belső margó 6, 4px réssel a mező alatt.
- Elem magasság 32–34, radius 3, belső margó 10; hover `hover`; kiválasztott elem előtt 6px `accent` pont vagy pipa.
- Esc zár, ↑↓ léptet, Enter választ.

## 8. Csúszka

- Vályú: teljes szélesség, magasság 6, radius 3, kitöltés `sunken`, keret 1px `line`.
- Kitöltött rész: `accent`, radius 3, bal széltől a hüvely középpontjáig.
- Hüvely: 22 átmérő, kitöltés `raised`, keret 1px `line`, `shadow_sm`.
- Kattintás a vályúra → oda ugrik és fogja a hüvelyt (pointer capture).
- ←→ = ±2 egység, Home/End = min/max. Érték mono fonttal a címke jobb szélén.
- A teljes sáv magassága `control_h` (a hitbox miatt), a vályú középen.

## 9. Folyamatjelző

- Determinált: magasság 8, radius 4, kitöltés `sunken`, keret 1px `line`, kitöltés `accent` balról.
- Indeterminált: ugyanaz, benne 33% szélességű `accent` szakasz, x: −100% → 300%, 1400ms ease-in-out, ciklikus.
- Spinner: 20 átmérő, 2px vonal `line`, ebből a felső 90° `accent`, 850ms lineáris körbe.
- Státuszpont: 8px kör — fut: `accent` kitöltés; leállítva: üres kör 1px `line`.

## 10. Scrollbar

- Szélesség 12, kitöltés `tray`, bal oldalán 1px `line_soft` elválasztó.
- Hüvely: 2px inset mindkét oldalon (tehát 8 széles), radius 4, `muted` 70% alfával.
- Hüvely magassága = `viewport² / content`, minimum 28. Pozíció = `scroll / scroll_max * (viewport − thumb)`.
- Kattintás a vályúra → a hüvely középpontja odaugrik, majd fogás. Nincs nyíl-gomb a végeken.
- Nem tűnik el, nem overlay: a lista szélességéből vesz el helyet (kiszámítható layout).

## 11. Lista / táblázat

- Fejléc: magasság 30 (`table_header_h`), kitöltés `tray`, alatta 1px `line`, szöveg label font `muted`.
- Sor: magasság `row_h`, alatta 1px `line_soft`. Oszlopok: pid 60 (mono, `muted`), név flex, méret 100 (mono), állapot 90 (mono, `muted`); belső margó 14, oszloprések 10.
- hover: kitöltés `hover`. **kijelölt:** kitöltés `sel_bg`, szöveg `sel_fg` (a mono oszlopok is).
- Lábléc (opcionális): magasság 30, `tray`, mono 11px `muted`, bal oldalon összegzés, jobbra méret.
- **üres állapot:** 1px szaggatott `line` keret, radius 4, 14px belső margó, középre igazított 14px szöveg `muted` + egy `accent` hívó szöveg.

## 12. Ablak

- Törzs: kitöltés `surface`, keret 1px `line`, radius 4, `shadow_lg`.
- Címsor: magasság 38, kitöltés `raised`, alatta 1px `line`, radius felül 3. Cím: body font weight 500, bal oldalon 10px margó; másodlagos infó (útvonal) mono, `muted`.
- Ablakgombok: 26×26, radius 3, kitöltés `tray`, glyph `text`; hover `hover`; a bezáró hoverben `accent` kitöltés + `on_accent` glyph.
- Nem aktív ablak: a címsor kitöltése `surface` helyett `tray`, a cím `muted`, árnyék `shadow_md`.

## 13. Menüsáv és menü

- Sáv magassága 34 (felső panel) vagy 36 (ablakon belül); elem magasság 26–28, radius 3, belső margó 10–12.
- Nyitott gyökérelem kitöltése `hover`.
- Popup: `raised`, 1px `line`, radius 4, `shadow_lg`, belső margó 6, minimum szélesség 236.
- Elem: magasság 34, radius 3, belső margó 10; balra címke, jobbra gyorsbillentyű mono 11px `muted`.
- Szeparátor: 1px `line_soft`, 5px felül/alul, 6px vízszintes margó.
- Alárendelt menü: jobbra 10px `▶` glyph, `muted`.
- Letiltott elem: 45% alfa, nincs hover.
- F10 fókuszálja a sávot, ←→ vált gyökérelemet, ↑↓ léptet, Esc zár.

## 14. Dialógus

- Modális háttér: fekete 45% alfa a teljes képernyőn.
- Panel: szélesség 380–440, belső margó 20, elemek közti rés 12, `raised`, 1px `line`, radius 4, `shadow_lg`.
- Cím: Barlow Condensed 600, 22px, ls 0.02em. Törzs: 14px, 1.5 sormagasság, `muted`; a benne szereplő útvonal mono + `text`.
- Opcionális infósor: `sunken` kitöltés, 1px `line`, bal oldalon 3px `accent` sáv, mono 12px `muted`.
- Műveletsor: jobbra igazítva, rés 8, gombmagasság 36; a megerősítő gomb primary, jobb szélen.
- Esc = mégse, Enter = megerősítés, a fókusz belépéskor a megerősítő gombon; a fókusz a dialóguson belül csapdázva.

## 15. Értesítés (toast)

- Szélesség 300, belső margó 14, `raised`, 1px `line`, bal oldalon 3px `accent` szegély, radius 4, `shadow_md` (a demóban `shadow_lg`).
- Cím: body 15px weight 500. Részlet: mono 13px `muted`.
- Folyamatos művelethez balra 20px spinner, 12px réssel.
- Automatikus eltűnés 6s után; egymás fölött 12px réssel, jobb alsó sarok, a taskbar felett 62px.

## 16. Taskbar / felső panel

- Felső panel: magasság 34, `raised`, alatta 1px `line`. Bal oldalon a rendszernév (Barlow Condensed 700, 14px, UPPERCASE, ls 0.14em) + menüsáv; jobb oldalon mono 12px `muted` státuszok 16px réssel, az óra `text` színnel.
- Taskbar: magasság 46, `raised`, felette 1px `line`. Ablakgomb: magasság 32, radius 4, belső margó 14; aktív = primary gomb, inaktív = `tray` kitöltés + 1px `line`.

---

## Billentyűzet-térkép (kötelező)

| billentyű | hatás |
|---|---|
| Tab / Shift+Tab | következő/előző vezérlő, gyűrű mindig látszik |
| Space | checkbox, switch kapcsolása; gomb aktiválása |
| Enter | gomb, menüelem, dialógus megerősítése |
| ←→ | radio, szegmentált, tab, csúszka léptetése |
| ↑↓ | lista- és menüelem léptetése |
| Home / End | csúszka min/max, lista első/utolsó eleme |
| PgUp / PgDn | lista egy viewportot lép |
| F10 | menüsáv fókuszálása |
| Esc | menü, dropdown, dialógus bezárása |
| Ctrl+N / T / M / F / Q | a demóban használt alkalmazás-szintű gyorsbillentyűk |

Az egész felület használható egér nélkül; egyetlen widget sem támaszkodik hoverre információ közlésében.

---

## Ha nincs antialiasing / alfa-blend

A design szándéka megőrizhető pixelrácsos rajzolóval is:

- **Radius 4 → sarok-levágás:** minden sarokban hagyj ki 1 pixelt (a 4px radius helyett 1px lépcső). A switch marad stadion-forma: két 26px magas félkör helyett rajzolj 26×26 „kerek” maszkot előre generált 1-bites maszkból (a maszkot fordítsd elő build-time-ban).
- **Kör alakú elemek** (radio, spinner, csúszka-hüvely, switch-hüvely): előre generált 1-bites maszkok 20/22/18 px-re, az élen 1px `line` színnel — így nem kell futásidejű AA.
- **Árnyékok → nincs blur:** helyettesítsd 1px `line` kerettel + 1px eltolt `line_soft` sávval a jobb és alsó élen. Modális háttér: 45% alfa helyett 50%-os sakktábla-dither `#000000`-nal, vagy egyszerű `line_soft` fátyol.
- **Fókuszgyűrű:** alfás `focus` helyett **tömör 2px `accent`** gyűrű, 1px offsettel.
- **Kitöltött accent felületen a hover/pressed** brightness helyett: hover = `accent` (változatlan) + 1px `on_accent` belső él; pressed = `press` tömör szín.

## Ha csak bitmap font van

- Body 15px → a legközelebbi elérhető 14–16px arányos font; mono 14px → 8×16 fix szélességű font kiválóan illik (a design mono szerepe: útvonal, pid, méret, gyorsbillentyű).
- A label-stílus (Barlow Condensed 11px UPPERCASE ls 0.14em) helyett: a meglévő legkisebb font NAGYBETŰKKEL, 1px extra betűréssel. A condensed hatás nem kritikus, a **nagybetű + betűrés + `muted` szín** viszi a jelentést.
- Címekhez (22/26px) egy második, nagyobb bitmap méret kell — ha nincs, használd a body fontot weight-különbség nélkül, de a `muted`/`text` kontrasztot tartsd meg.
