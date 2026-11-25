
# DragonSense

DragonSense is a capstone project focused on improving the training experience for the University of Waterloo Dragon Boat Club (UWDBC).

The system aims to help athletes and coaches capture workout data, understand paddling performance, and track progress over time using the Paddle Ergometer (PERG).

## Repository Structure

```text
dragonsense/
├── mobile/                 React Native mobile app
│   ├── src/
│   ├── assets/
│   └── package.json
│
├── firmware/
│   └── perg_logger/        PERG firmware and data tools
│
└── README.md
```

## Mobile App

The mobile application is built with:

- React Native
- Expo
- TypeScript
- Firebase

### Getting Started

From the repository root, run:

```bash
cd mobile
npm ci
npx expo start
```

See [mobile/README.md](mobile/README.md) for more information.

## Firmware

The firmware contains PERG sensor and display firmware, along with Python tools for:

- Capturing flywheel and foot sensor data
- Recording workout telemetry
- Analyzing paddling performance
- Replaying recorded or synthetic signals

The firmware currently uses an ESP32 sensor board and a Freenove ESP32-S3 display.

See [firmware/perg_logger/README.md](firmware/perg_logger/README.md) for hardware setup, recording instructions, dependencies, and development checks.

## Development

The mobile application and firmware are maintained as separate components within this repository. Each component has its own dependencies and setup instructions.
