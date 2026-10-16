# Installing software on d-os

This guide is for **people using d-os**. It shows how to download a program from
the people who make it, install it, keep it up to date and remove it.
Docker and Java are the worked examples. Everything happens on the d-os
machine itself: you do not need another computer and you do not need an image
registry.

> Magyarul: [TELEPITES.md](TELEPITES.md).

## What you need

- A **persistent disk**, mounted at `/mnt`. Installed software lives in
  `/mnt/apps`, so it survives a reboot. A machine with no disk can still
  install software, but the installation is gone after a reboot.
- **Room on that disk.** You need space for the download *and* for the
  unpacked program:
  - Docker: about 90 MB to download, 280 MB unpacked.
  - Docker's build plugin (buildx): about 70 MB.
  - A JDK (Java): about 200 MB to download, 330 MB unpacked.
  - Images you pull with Docker need room too.
- **A network connection.** `dhcp` sets it up on a normal machine; `netstate`
  shows whether you have one.
- **The right build for your machine.** `x86_64` (also called amd64 or x64) or
  `aarch64` (also called arm64). The `arch` command tells you which one you have.
  If you install the wrong one, the installer refuses it and says why.

## The three steps

### 1. Download the package

```
wget <the vendor's download URL> /mnt
```

The file is saved in `/mnt` under its own name. Notes:

- Redirects are followed. GitHub and Adoptium links start with one.
- The download is checked against the length the server announced. If the
  connection breaks part-way, `wget` continues from where it stopped (up to
  five times). If it still cannot finish, it reports an error and removes the
  half file.
- An error page from the server (404 and similar) is never saved as the
  package.

To pick the file name yourself, give a file path instead of `/mnt`:
`wget <url> /mnt/jdk.tar.gz`.

### 2. Install it

```
app install /mnt/<file> [name]
```

`app install` takes three kinds of file:

| what you downloaded | example |
|---|---|
| a `.tar.gz`, `.tgz` or `.tar` release | `docker-29.9.1.tgz`, a JDK tarball |
| a single executable | `buildx-v0.38.0.linux-amd64` |
| an OCI image archive (`docker save`) | `java.tar` |

The **name** and **version** come from the file name: `docker-29.9.1.tgz`
becomes `docker` version `29.9.1`. When the file name does not give a useful
name, add one yourself, for example `app install /mnt/jdk.tar.gz java`.

Before installing, every program in the package is checked to see whether it
can run on this machine:

- A program **built for another processor** is refused. For example: *built
  for x86_64 - this machine is aarch64*.
- A **glibc-linked** program runs with the glibc package that d-os ships. It
  comes from Debian 12 and is unmodified. A **musl** build (Alpine) or a
  **static** build runs as it is.
- If anything cannot run, **nothing is installed**. The unpacked files are
  removed and the reason is printed.

The installed files go to `/mnt/apps/<name>/<version>/`. The programs are
linked into `/bin`, so you can run them by name.

### 3. Use it

Run the programs by name: `docker`, `java`, `javac`.

Some software needs more than its programs on `PATH`. Docker, for example,
needs two background services. d-os knows what Docker and buildx need, and a
package can also carry its own `.dos-app` file (see below). The installer does
these steps for you right after installing, and again at every boot:

- it starts the services,
- it creates the directories they expect,
- it turns on IPv4 forwarding,
- it puts plugin links in place.

## Worked example: Docker

```
wget https://download.docker.com/linux/static/stable/x86_64/docker-29.9.1.tgz /mnt
app install /mnt/docker-29.9.1.tgz
wget https://github.com/docker/buildx/releases/download/v0.38.0/buildx-v0.38.0.linux-amd64 /mnt
app install /mnt/buildx-v0.38.0.linux-amd64
docker version
docker run --rm busybox:musl echo hello
```

On aarch64, use `.../aarch64/docker-29.9.1.tgz` and
`buildx-v0.38.0.linux-arm64` instead.

What happens:

- `app install` for Docker starts `containerd`, waits until its socket exists,
  and then starts `dockerd`.
- dockerd uses the `overlay2` storage driver and the `nftables` firewall
  backend. `nft` is part of d-os.
- At every boot both services start again by themselves.
- `docker build` uses buildx (BuildKit). Without buildx, use
  `env DOCKER_BUILDKIT=0 docker build ...`, the older builder.
- Containers run with Docker's default seccomp profile. Inside a container,
  `/proc/self/status` reports `Seccomp: 2`.

**Known limitation:** images, containers and volumes live in `/var/lib/docker`,
which is in memory. They are gone after a reboot. The exFAT disk cannot hold
Docker's overlay layers (no device nodes, no extended attributes). The Docker
programs themselves stay installed.

## Worked example: Java (Temurin JDK)

```
wget https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse /mnt/temurin21.tar.gz
app install /mnt/temurin21.tar.gz java
java -version
```

You can use either JDK build:

- the normal `linux` build, which runs on d-os's glibc package, or
- the `alpine-linux` build (musl).

On aarch64, use `.../linux/aarch64/...` in the URL.

## Updating, switching and removing

```
app list                       # everything installed; * marks the active version
app install /mnt/<newer file>  # installs BESIDE the old version and makes it active
app use <name> <version>       # switch back (or forward)
app remove <name> <version>    # remove one version
app remove <name>              # remove the application entirely
app start <name>               # re-apply its manifest (start its services again)
```

- An update **never overwrites** the old version, so going back is one command.
- If an install is interrupted (for example the power goes), it is detected at
  the next attempt and done again from the start.

## A package's own manifest (`.dos-app`)

A release can carry a file called `.dos-app` in its top directory. It has one
instruction per line. These run after the install and at every boot:

```
dir=/run                                  # make a directory
sysctl=/proc/sys/net/ipv4/ip_forward 1    # set a kernel setting
link=bin/tool /usr/local/bin/tool         # a link to a file of the package
service=daemon --flag                     # start a background program
wait=/run/daemon.sock 60                  # wait up to 60 s for a file to appear
program=tool                              # (single executable) the name to install it as
```

## When something fails

Every refusal says why. The common ones:

| message | what to do |
|---|---|
| `built for x86_64 - this machine is aarch64` | download the build for your processor |
| `linked against GLIBC ... install the glibc package first` | your d-os build has no glibc package (`scripts/fetch-glibc.sh`); or download the musl/alpine build |
| `the server answered 404 - nothing saved` | the URL is wrong |
| `the connection ended after N of M bytes - incomplete` | `wget` resumes by itself; if it gives up, run the same `wget` again |
| `cannot be created - installed software needs the persistent disk` | attach a disk |
| `write failed (disk full?)` | free space, or use a bigger disk |

To see the whole story, look at the console output, or run `dmesg`.
