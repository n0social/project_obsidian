# Play project_obsidian on a tablet

project_obsidian is the step on the tablet between you and a private Vanilla **1.12.1** server. The server stays where you already run it. This app loads your game files, connects to that server, and plays on the tablet.

The app ships no Blizzard game files and no built-in realm. You supply your own legally obtained 1.12.1 client, and an account that already exists on your server.

This is an early build. A session can still drop after you enter the world, and character appearance is still rough.

## What you need

- An Android tablet. Landscape, with a USB or Bluetooth keyboard and mouse, is the setup this build is aimed at. It has been tried on a Galaxy Tab A9. Android 11 or newer.
- About 12 GB free on the tablet if you extract on the device. The packed archives and the unpacked game files both sit on the tablet until extract finishes.
- Your own 1.12.1 client folder. The login screen of that client shows `Version 1.12.1.5875`. The folder contains `WoW.exe` and a `Data` folder full of `.MPQ` files.
- A private 1.12.1 server the tablet can reach. Use that server’s LAN address or public hostname. On the tablet, `localhost` means the tablet itself.
- The username and password for an account on that server. Vanilla accounts are usually a username, not an email address.
- The debug APK from this repo: [`obsidian-debug.apk`](obsidian-debug.apk).

## 1. Install the app

1. Copy `obsidian-debug.apk` onto the tablet, or download it from this repository in the tablet’s browser.
2. Open the APK and allow installation from that source if Android asks.
3. Open **project_obsidian**. The first screen is landscape, with a list on the left: Home, Sign in, Graphics, Audio, Controls, Data, About, and **Enter World** at the bottom.

## 2. Add your game files

Open **Data**. Do one of the two steps below, not both.

The page shows whether the built-in extractor is ready, and whether `manifest.json` is already present.

### Extract on the tablet

Use this when you have the original 1.12.1 folder and have not unpacked it yet.

1. Tap **Select WoW client folder and extract**.
2. Choose the folder that contains `Data` and the `.MPQ` archives, or choose the `Data` folder itself.
3. Leave project_obsidian open. Copying the archives comes first, then the built-in extractor unpacks them. A full extract often takes 10–30 minutes.
4. Wait until the status says the classic data is ready. **Cancel** stops the job.

### Import a folder you already unpacked

Use this when a PC extract already produced a folder that contains `manifest.json`.

1. Tap **Import extracted Data**.
2. Choose that unpacked folder.
3. Wait until the copy finishes.

## 3. Point the tablet at your server

Open **Sign in**.

1. **Hostname:** the address of your server, such as `192.168.1.20` or `auth.example.com`.
2. **Port:** your server’s auth port. The field starts at `3724`, which is the usual Vanilla auth port. Change it if your server uses another port.
3. **Username** and **Password:** the account for this server. **Show password** reveals the password while you type.

Enter World is what saves these fields.

## 4. Enter the world

1. Optional: open **Graphics**, **Audio**, or **Controls** first. Graphics apply when you enter the world. A preset fills the detailed controls. Controls covers keyboard and mouse, screen-on, and landscape lock. Relative mouse look is for the in-world camera when a mouse is connected.
2. Tap **Enter World**.
3. If the hostname, username, or password is empty, the app returns to Sign in and tells you what to fill in.
4. If game files are missing, the app returns to Data.
5. When the fields are filled and the game files are present, confirm **Enter World**. The tablet then connects to `hostname:port` with that account.

Home shows whether game files were found and which host Enter World will use.

## After you are in

- The world session can still disconnect. Character hair and appearance can still look wrong. Those are known limits of this build.
- Graphics → **Texture resolution** defaults to Medium (512) on a tablet. That softens textures and uses less memory.
- Your username and password stay in the app’s private storage on that tablet. They are not part of the APK or this repository.

## If something fails

| What you see | What to do |
|---|---|
| Built-in extractor: unavailable | Use **Import extracted Data** with a folder that already contains `manifest.json`. |
| No `.MPQ` files found | Select the 1.12.1 install folder, or its `Data` folder. |
| Selected folder is not an extracted classic tree | The import folder needs `manifest.json`. |
| Enter your realm hostname | Sign in is missing a hostname. Use the server’s address, not the tablet’s `localhost`, unless the server is actually running on the tablet. |
| Classic Data missing | Finish Data → extract or import, then Enter World again. |
| Extract seems stuck | Keep the app in the foreground. A full extract is long. Free space has to cover both the archives and the unpacked files. |

Developer build steps live in [README.md](README.md). This page is the one to follow if you already have the APK.
