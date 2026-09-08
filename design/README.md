# Handoff: Console Plate — OS widget készlet

## Áttekintés

Egy hobbi/tanuló operációs rendszer natív widget-nyelve: gombok, checkbox/radio/switch, szövegbevitel, dropdown, csúszka, folyamatjelző, scrollbar, lista/táblázat, ablakkeret, menüsáv és menü, dialógus, értesítés, taskbar. Két téma (dark alapértelmezett, light) és két sűrűség (40px / 32px vezérlőmagasság) — **azonos geometria, csak a token-réteg cserél**, hogy a téma futásidőben egy struktúra-csere legyen.

Cél: ergonomikus, egér nélkül is teljesen használható, mérnöki-technikai karakterű felület, amely 1px-es keretekkel és négy felületréteggel épül — nem gradiensekkel.

## A design fájlokról

A `reference/` mappában lévő fájlok **HTML-ben készült design referenciák** — prototípusok, amelyek a kívánt megjelenést és viselkedést mutatják. **Nem** production kód, és nem a HTML-t kell bevinni az OS-be. A feladat: ezt a látványt és viselkedést újraalkotni a cél-környezetben, ami itt egy **saját framebuffer rajzoló C-ben** — azaz a specifikációból (`console_plate.h` + `widget_specs.md`) kell megírni a widget-rajzoló és esemény-kezelő kódot.

A HTML referenciát böngészőben nyisd meg, és kattints végig: a fejlécben téma (Dark/Light), sűrűség (40/32px) és nézet (Katalógus/Demó) váltó van. A vezérlők élnek — checkbox, radio (nyilakkal is), switch, szegmentált, tabok, dropdown, csúszka (fogható + nyilak), scrollbar (fogható + görgő), menü, modális dialógus.

## Fidelity

**High-fidelity.** A színek, méretek, tipográfia és állapotok véglegesek, pixelre megadva a `tokens.json` / `console_plate.h` / `widget_specs.md` fájlokban. A rajzolt eredménynek ezeket kell követnie — a HTML csak a vizuális ellenőrzés eszköze.

## Nyitott technikai kérdések (a designt nem blokkolják)

A designer nem tudta, hogy a rajzoló tud-e antialiasingot/alfa-blendet, és hogy van-e TrueType renderelés. Ezért a spec **mindkét esetre** ad utat:

- `widget_specs.md` → *„Ha nincs antialiasing / alfa-blend”* szakasz: sarok-levágás radius helyett, előre generált 1-bites körmaszkok, árnyék helyett 1px élek, dither a modális háttérhez, tömör accent fókuszgyűrű.
- `widget_specs.md` → *„Ha csak bitmap font van”* szakasz: 8×16 fix font a mono szerepre, nagybetű + betűrés a condensed label-stílus helyett.

Ha kiderül, hogy van AA + alfa és TrueType, az „elsődleges” értékek érvényesek (radius 4, blur-os árnyékok, alfás fókuszgyűrű, Barlow / Barlow Condensed / IBM Plex Mono).

Felbontás nem volt megadva: **minden érték logikai pixel 1× skálán**. HiDPI-hez egész számú skálázás (`CP_SCALE` makró a headerben) — a hosszméretek skálázódnak, a színek nem. A layoutok minimum 1024×768-on működnek; a demó képernyő 1400×860-on van kirakva.

## Nézetek

### 1. Katalógus (`view = catalogue`)

Fejléc (sticky, 34–48px, `raised` kitöltés, 1px `line` alatta): rendszernév + három tálcás váltó (téma, sűrűség, nézet).

Szakaszok, mindegyik egy 1px `line` alsó szegélyű címsorral (mono sorszám `accent` színnel + Barlow Condensed 26px UPPERCASE cím + `muted` alcím):

1. **Alapok** — a négy felületréteg + accent + hover swatchei 56px magas, 1px keretes, radius 4 mezőkben; alatta mono spec-sor (radius, border, control, row, scrollbar, fókusz, tipográfia).
2. **Gombok** — 4 hangsúly (primary/secondary/ghost/ikon) × 5 állapot (alap/hover/pressed/fókusz/letiltva) rácsban, plusz egy összevont gombcsoport.
3. **Választók** — checkbox (be/ki/részleges/letiltva), radio 3 opcióval, switch 2 sorral + állapotszöveg mono fonttal, szegmentált kontroll.
4. **Bevitel** — csatolási pont (mono), címke + karakterszámláló, **hibás** blokkméret (accent keret + bal sáv + hibaüzenet), letiltott eszköz, dropdown, kereső mező.
5. **Értékek** — csúszka min/max jelöléssel, determinált és indeterminált folyamatjelző, spinner, státuszpontok.
6. **Navigáció és lista** — tabok, 12 soros táblázat fejléccel/lábléccel, kijelölt sorral, saját scrollbarral; üres állapot; menü popup alárendelt szinttel és letiltott elemmel; elérési út sáv.
7. **Átfedő elemek** — dialógus infósorral, két értesítés (statikus és spinneres), tooltip, billentyűzet-térkép.

### 2. Demó (`view = demo`)

Egy 1400×860 asztal, `bg` kitöltés + 45°-os `line_soft` sraffozás (0.6 alfa) mint háttér:

- **Felső panel** (34px): rendszernév, menüsáv (Fájl/Nézet/Súgó — a Fájl nyitható), jobb oldalon mono státuszok (load, mem, eth0, óra).
- **Rendszerfigyelő ablak** (820px, bal felül): címsor 3 ablakgombbal, eszköztár tabokkal + kereső mezővel, táblázat fejléc, 7 sor magas görgethető lista kijelölt sorral (pid 377), lábléc összegzéssel és két gombbal (Prioritás… / Leállítás → dialógus).
- **Kernel beállítások ablak** (440px, jobb oldalon): két switch, csúszka (Swappiness), dropdown (Ütemező).
- **Értesítés** jobb alul a taskbar felett 62px-re.
- **Taskbar** (46px): három ablakgomb (az aktív primary), jobbra csomagtelepítés folyamatjelzője + mono felirat.
- **Modális dialógus**: fekete 45% fátyol, 380px panel, Esc/Enter kezeléssel.

## Interakciók és viselkedés

- **Checkbox/switch:** klikk vagy Space/Enter kapcsol; switch hüvely 120ms lineáris eltolás (3 → 25px).
- **Radio:** klikk választ; ↑↓/←→ körkörösen léptet a csoporton belül; a csoport egyetlen tab-stop.
- **Szegmentált/tab:** klikk vagy ←→; az aktív elem `raised` kitöltést kap.
- **Dropdown:** klikk nyit/zár, elem választása zár; Esc zár, ↑↓ léptet, Enter választ. A lista `shadow_lg`-vel a mező alatt 4px réssel.
- **Csúszka:** pointer down bárhol a sávon → azonnal odaugrik és fogja (pointer capture a felengedésig); ←→ ±2, Home/End min/max.
- **Scrollbar:** görgő = `deltaY * 0.5` px; a vályúra kattintás a hüvely közepét odarakja, majd fog; nincs végnyíl. Hüvely = `viewport²/content`, min 28px.
- **Menü:** gyökérelem klikkre nyit (nyitva a kitöltése `hover`), Esc zár, F10 fókuszál; menüelem klikk után zár (a demóban a „Partíció formázása…” dialógust nyit).
- **Dialógus:** fókusz-csapda a panelen belül, belépéskor a megerősítő gombon; Esc = mégse, Enter = megerősítés.
- **Toast:** 6s után magától eltűnik; több toast egymás fölött 12px réssel.
- **Fókusz:** minden vezérlőn 2px `focus` gyűrű, 1px offset (inputon offset 0 + `accent` keret; switch-sornál offset 2). Sose maradjon fókuszjelzés nélküli vezérlő.
- **Téma/sűrűség váltás:** azonnali, 140ms színátmenettel; a layout nem ugrik, mert a geometria a sűrűségtől csak `control_h` / `row_h` / betűméret szintjén függ.

## Állapot (mit kell tárolni)

Widget-szinten: `checked` (checkbox/switch), `group_index` (radio/szegmentált/tab), `value` (csúszka 0–100, input string), `open` (dropdown/menü), `scroll_offset` + `selected_row` (lista), `focused` + `hovered` + `pressed` (minden interaktív elem), `enabled`.

Alkalmazás-szinten a demóhoz: aktív tab, kijelölt folyamat, nyitott menü, nyitott dialógus, téma, sűrűség.

A referenciában ugyanez a modell fut (`Console Plate.dc.html` logikai osztálya) — a `renderVals()` mutatja, mely állapotból mi következik vizuálisan.

## Design tokenek

Teljes lista: `tokens.json` (nyelvfüggetlen) és `console_plate.h` (C, `0xAARRGGBB`, `cp_theme` / `cp_density` struktúrák + geometria makrók). Kivonat:

| token | dark | light |
|---|---|---|
| bg | `#16232f` | `#e7e7ea` |
| surface | `#1d2d3d` | `#f5f5f8` |
| sunken | `#16232f` | `#ffffff` |
| raised | `#22374a` | `#ffffff` |
| tray | `#1a2937` | `#e7e7ea` |
| line | `#416180` | `#d4d4d7` |
| line_soft | `#2c455d` | `#e7e7ea` |
| text | `#eef6ff` | `#1d1f20` |
| muted | `#94bce3` | `#5d5d60` |
| accent | `#94bce3` | `#5980a6` |
| on_accent | `#16232f` | `#ffffff` |
| hover / press | `#2c455d` / `#416180` | `#eef6ff` / `#d6ebff` |
| sel_bg / sel_fg | `#416180` / `#eef6ff` | `#d6ebff` / `#1d1f20` |

Geometria: border 1 · radius 4 (belső 3) · control 40/32 · row 40/32 · checkbox 20 · radio 20 (pont 10) · switch 48×26 (hüvely 18, út 22) · csúszka sáv 6 / hüvely 22 · progress 8 · spinner 20 · scrollbar 12 (hüvely min 28) · címsor 38 · panel 34 · taskbar 46 · ablakgomb 26 · menüelem 34 · tab 32 · fókusz 2+1.

Tipográfia: **Barlow** 15/14px body (1.45 sormagasság), **Barlow Condensed** 600 a címekhez (22px dialógus, 26px szakasz) és a label-stílushoz (11px UPPERCASE, 0.14em betűrés), **IBM Plex Mono** 14/13px minden gépi értékhez (útvonal, pid, méret, állapot, gyorsbillentyű).

Mozgás: switch 120ms lineáris · téma 140ms · spinner 850ms lineáris · indeterminált sáv 1400ms ease-in-out.

## Assetek

Nincs kép- vagy ikon-asset. A referencia HTML néhány karakter-glyphet használ helyettesítőként (`–` `□` `×` `▼` `▶` `✓`) — a natív implementációban ezeket **rajzolt 1px-es vonalgrafikaként** vagy előre generált 1-bites ikonmaszkként érdemes megoldani, Lucide-szerű, vékony (1.5px arányú) vonalvezetéssel. Az ikonrács 20×20, a glyph optikai súlypontja középen.

## Fájlok

- `tokens.json` — minden token nyelvfüggetlenül.
- `console_plate.h` — C header: `cp_theme` (dark/light), `cp_density` (comfort/compact), geometria makrók, `CP_SCALE`.
- `widget_specs.md` — widgetenként pixelgeometria, minden állapot, rajzolási sorrend, találati terület; billentyűzet-térkép; AA nélküli és bitmap-font fallback.
- `reference/Console Plate.dc.html` — az élő referencia (katalógus + demó, téma/sűrűség váltó). Böngészőben nyitható; a mellette lévő `support.js`, `ds-styles.css` és `_ds/…/styles.css` kell hozzá.
- `reference/OS UI Kit.dc.html` — az eredeti három feltárt irány (Blueprint / Console / Plate) és a belőlük összeolvasztott Console Plate pár. Csak kontextus: miért így néz ki a végleges nyelv.

## Első lépések javaslata Claude Code-hoz

1. Vidd be a `console_plate.h`-t, és köss egy globális `const cp_theme *theme` + `const cp_density *dens` mutatót — a téma- és sűrűségváltás így egy értékadás.
2. Írj primitíveket előbb: `fill_rect`, `fill_round_rect(radius)`, `stroke_rect(1px)`, `fill_circle_mask`, `draw_text(font, color, align)`, `draw_shadow(shadow, rect)` — a `widget_specs.md` rajzolási sorrendje ezekre épül.
3. Utána widgetenként haladj a spec sorrendjében (gomb → checkbox → radio → switch → tab → input → dropdown → csúszka → progress → scrollbar → lista → ablak → menü → dialógus → toast).
4. Minden widgetnél legelőbb az **állapotsort** kösd be (hover/pressed/focus/disabled) — utólag beépíteni sokkal drágább.
5. A billentyűzet-kezelés ne widget-specifikus hack legyen: egy fókuszlánc (tab-order) + widgetenkénti `handle_key` a spec táblája szerint.
