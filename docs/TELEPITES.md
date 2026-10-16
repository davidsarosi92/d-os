# Szoftver telepítése d-os-ra

Ez az útmutató **a d-os felhasználóinak** szól. Megmutatja, hogyan tölts le egy
programot közvetlenül a készítőjétől, hogyan telepítsd, frissítsd és távolítsd
el. A példák a Docker és a Java. Minden a d-os gépen történik: nem kell hozzá
másik számítógép, és image-regiszter sem.

> English: [INSTALLING.md](INSTALLING.md).

## Mi kell hozzá

- **Tartós lemez** a `/mnt` alatt. A telepített szoftver a `/mnt/apps`-ba
  kerül, így megmarad újraindítás után is. Lemez nélkül is lehet telepíteni,
  de a telepítés újraindításkor elvész.
- **Hely ezen a lemezen**, a letöltésnek *és* a kibontott programnak is:
  - Docker: kb. 90 MB letöltés, 280 MB kibontva.
  - A Docker build-pluginja (buildx): kb. 70 MB.
  - JDK (Java): kb. 200 MB letöltés, 330 MB kibontva.
  - A Dockerrel lehúzott image-eknek is kell hely.
- **Hálózat.** Szokásos gépen a `dhcp` beállítja; a `netstate` megmutatja,
  van-e.
- **A gépednek megfelelő kiadás**: `x86_64` (más néven amd64, x64) vagy
  `aarch64` (más néven arm64). Az `arch` parancs megmondja, melyik a tiéd.
  Ha rosszat választasz, a telepítő megtagadja, és megírja, miért.

## A három lépés

### 1. Töltsd le a csomagot

```
wget <a gyártó letöltési URL-je> /mnt
```

A fájl a saját nevén kerül a `/mnt`-be. Tudnivalók:

- Az átirányításokat a `wget` követi. A GitHub és az Adoptium linkjei így
  kezdődnek.
- A letöltés hosszát összeveti azzal, amit a szerver bejelentett. Ha a
  kapcsolat közben megszakad, a `wget` onnan folytatja, ahol abbamaradt
  (legfeljebb ötször). Ha így sem sikerül, hibát ír, és törli a félkész fájlt.
- A szerver hibaoldalát (404 és társai) soha nem menti el csomagként.

Ha te akarod megadni a fájl nevét, könyvtár helyett írj fájlnevet:
`wget <url> /mnt/jdk.tar.gz`.

### 2. Telepítsd

```
app install /mnt/<fájl> [név]
```

Az `app install` háromféle fájlt fogad el:

| mit töltöttél le | példa |
|---|---|
| `.tar.gz`, `.tgz` vagy `.tar` kiadás | `docker-29.9.1.tgz`, egy JDK-tarball |
| egyetlen futtatható fájl | `buildx-v0.38.0.linux-amd64` |
| OCI image archívum (`docker save`) | `java.tar` |

A **név** és a **verzió** a fájlnévből jön: a `docker-29.9.1.tgz` neve
`docker`, verziója `29.9.1`. Ha a fájlnévből nem jön ki értelmes név, add meg
te, például: `app install /mnt/jdk.tar.gz java`.

Telepítés előtt a telepítő a csomag minden programjáról ellenőrzi, hogy
futhat-e ezen a gépen:

- A **más processzorra fordított** programot megtagadja. Például: *built for
  x86_64 - this machine is aarch64*.
- A **glibc-hez linkelt** program a d-os glibc-csomagjával fut. Ez a Debian
  12-ből származik, módosítatlanul. A **musl** (Alpine) és a **statikus**
  kiadás önmagában fut.
- Ha bármi nem futhat, **semmi nem települ**. A kibontott fájlok törlődnek, és
  a telepítő kiírja az okot.

A telepített fájlok a `/mnt/apps/<név>/<verzió>/` alá kerülnek. A programok
linkjei a `/bin`-be kerülnek, így név szerint indíthatod őket.

### 3. Használd

Indítsd a programokat név szerint: `docker`, `java`, `javac`.

Van, aminek a `PATH`-on lévő programokon kívül más is kell; a Dockernek például
két háttérszolgáltatás. A d-os tudja, mi kell a Dockernek és a buildx-nek, és
egy csomag a saját `.dos-app` fájlját is hozhatja (lásd lent). A telepítő ezeket
a lépéseket elvégzi rögtön a telepítés után, és minden indításkor újra:

- elindítja a szolgáltatásokat,
- létrehozza a könyvtárakat, amiket várnak,
- bekapcsolja az IPv4-továbbítást,
- elhelyezi a plugin-linkeket.

## Példa: Docker

```
wget https://download.docker.com/linux/static/stable/x86_64/docker-29.9.1.tgz /mnt
app install /mnt/docker-29.9.1.tgz
wget https://github.com/docker/buildx/releases/download/v0.38.0/buildx-v0.38.0.linux-amd64 /mnt
app install /mnt/buildx-v0.38.0.linux-amd64
docker version
docker run --rm busybox:musl echo hello
```

aarch64-en a `.../aarch64/docker-29.9.1.tgz` és a
`buildx-v0.38.0.linux-arm64` kell helyettük.

Mi történik:

- A Docker telepítésekor az `app install` elindítja a `containerd`-t, megvárja,
  amíg megjelenik a socketje, aztán elindítja a `dockerd`-t.
- A dockerd az `overlay2` tárolót és az `nftables` tűzfal-backendet használja.
  Az `nft` a d-os része.
- Minden indításkor mindkét szolgáltatás magától elindul.
- A `docker build` a buildx-et (BuildKit) használja. Ha nincs buildx, akkor
  `env DOCKER_BUILDKIT=0 docker build ...` a régi builderrel működik.
- A konténerek a Docker alapértelmezett seccomp-profiljával futnak. A
  konténerben a `/proc/self/status` ezt írja: `Seccomp: 2`.

**Ismert korlát:** az image-ek, a konténerek és a kötetek a
`/var/lib/docker`-ben vannak, ami a memóriában van. Újraindításkor elvesznek.
Az exFAT lemez nem tudja tárolni a Docker overlay-rétegeit (nincsenek rajta
eszköz-csomópontok és kiterjesztett attribútumok). Maguk a Docker-programok
telepítve maradnak.

## Példa: Java (Temurin JDK)

```
wget https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse /mnt/temurin21.tar.gz
app install /mnt/temurin21.tar.gz java
java -version
```

Bármelyik JDK-kiadás jó:

- a szokásos `linux` kiadás, ami a d-os glibc-csomagjával fut, vagy
- az `alpine-linux` kiadás (musl).

aarch64-en az URL-ben `.../linux/aarch64/...` legyen.

## Frissítés, váltás, eltávolítás

```
app list                       # minden telepített szoftver; * jelöli az aktív verziót
app install /mnt/<újabb fájl>  # a régi MELLÉ települ, és az lesz az aktív
app use <név> <verzió>         # visszaváltás (vagy előre)
app remove <név> <verzió>      # egy verzió törlése
app remove <név>               # az egész alkalmazás törlése
app start <név>                # a manifest újra (szolgáltatások újraindítása)
```

- A frissítés **soha nem írja felül** a régi verziót, így a visszalépés egyetlen
  parancs.
- Ha a telepítés megszakad (például áramszünet miatt), a következő
  próbálkozáskor a rendszer ezt észreveszi, és elölről kezdi.

## A csomag saját manifestje (`.dos-app`)

Egy kiadás a felső könyvtárában `.dos-app` nevű fájlt is hozhat. Soronként egy
utasítás van benne, és ezek a telepítés után és minden indításkor lefutnak:

```
dir=/run                                  # könyvtár létrehozása
sysctl=/proc/sys/net/ipv4/ip_forward 1    # kernelbeállítás
link=bin/tool /usr/local/bin/tool         # link a csomag egy fájljára
service=daemon --flag                     # háttérprogram indítása
wait=/run/daemon.sock 60                  # várakozás (legfeljebb 60 mp) egy fájlra
program=tool                              # (egyetlen futtatható) ezen a néven települjön
```

## Ha valami nem sikerül

Minden elutasítás megmondja az okát. A gyakoriak:

| üzenet | teendő |
|---|---|
| `built for x86_64 - this machine is aarch64` | töltsd le a processzorodnak megfelelő kiadást |
| `linked against GLIBC ... install the glibc package first` | a d-os buildedben nincs glibc-csomag (`scripts/fetch-glibc.sh`), vagy töltsd le a musl/alpine kiadást |
| `the server answered 404 - nothing saved` | hibás az URL |
| `the connection ended after N of M bytes - incomplete` | a `wget` magától folytatja; ha feladja, futtasd újra ugyanazt a `wget`-et |
| `cannot be created - installed software needs the persistent disk` | csatolj lemezt |
| `write failed (disk full?)` | szabadíts fel helyet, vagy használj nagyobb lemezt |

A teljes történetet a konzol kimenetén látod, vagy a `dmesg` paranccsal.
