# Welcome to your Expo app 👋

This is an [Expo](https://expo.dev) project created with [`create-expo-app`](https://www.npmjs.com/package/create-expo-app).

## Test live PERG collection

The **Live Test** tab uses native Bluetooth. Install a development build on
the phone. On a Mac with Xcode and a connected iPhone, run `npm ci` and
`npx expo run:ios --device` from `mobile/`. Expo Go can still open the
**Import CSV** screen for saved recordings.

1. Power the sensor ESP32 and the Freenove running the phone-relay sketch.
   Wait for the Freenove dashboard and disconnect nRF Connect if it is open.
2. Open **Live Test** and tap **Connect to Freenove**. Once the app shows
   that it is receiving data, tap **Start capture**. Press and release each
   pad in turn and compare the corresponding live reading with the LCD.
3. For a peak check, start a session on the LCD and row a few strokes.
   Compare the held total and both contributions on the phone and monitor.
4. Tap **Stop and save capture**, then **Share saved capture** to copy the
   file to Files or to your computer. Check that it contains several
   `stats` records with changing foot loads and, if you rowed, the expected
   `foot_peak` values.

The capture is a local `.ndjson` file. Each line contains the phone's receive
time in epoch milliseconds, the original telemetry line, and its parsed
fields. The file includes `stats`, `drive`, and `foot_peak` messages.

## Get started

1. Install dependencies

   ```bash
   npm install
   ```

2. Start the app

   ```bash
   npx expo start
   ```

In the output, you'll find options to open the app in a

- [development build](https://docs.expo.dev/develop/development-builds/introduction/)
- [Android emulator](https://docs.expo.dev/workflow/android-studio-emulator/)
- [iOS simulator](https://docs.expo.dev/workflow/ios-simulator/)
- [Expo Go](https://expo.dev/go), a limited sandbox for trying out app development with Expo

You can start developing by editing the files inside the **src/app** directory. This project uses [file-based routing](https://docs.expo.dev/router/introduction).

## Get a fresh project

When you're ready, run:

```bash
npm run reset-project
```

This command will move the starter code to the **example** directory and create a blank **src/app** directory where you can start developing.

### Other setup steps

- To set up ESLint for linting, run `npx expo lint`, or follow our guide on ["Using ESLint and Prettier"](https://docs.expo.dev/guides/using-eslint/)
- If you'd like to set up unit testing, follow our guide on ["Unit Testing with Jest"](https://docs.expo.dev/develop/unit-testing/)
- Learn more about the TypeScript setup in this template in our guide on ["Using TypeScript"](https://docs.expo.dev/guides/typescript/)

## Learn more

To learn more about developing your project with Expo, look at the following resources:

- [Expo documentation](https://docs.expo.dev/): Learn fundamentals, or go into advanced topics with our [guides](https://docs.expo.dev/guides).
- [Learn Expo tutorial](https://docs.expo.dev/tutorial/introduction/): Follow a step-by-step tutorial where you'll create a project that runs on Android, iOS, and the web.

## Join the community

Join our community of developers creating universal apps.

- [Expo on GitHub](https://github.com/expo/expo): View our open source platform and contribute.
- [Discord community](https://chat.expo.dev): Chat with Expo users and ask questions.
