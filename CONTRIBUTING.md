# Contributing to the VLSI RTOS Pipeline

Thank you for your interest in contributing! Since this project is heavily focused on hardware-software integration, we welcome pull requests that optimize the physical hardware interactions.

## How to Contribute
1. Fork the repository and create your feature branch (`git checkout -b feature/hardware-optimization`).
2. If adding new hardware probe functions, ensure they are pinned to **Core 1** to maintain real-time determinism.
3. Network or logging additions must remain on **Core 0**.
4. Commit your changes (`git commit -m 'Add DMA buffer support'`).
5. Push to the branch and open a Pull Request.

Please ensure all new IPC primitives are properly defended against priority inversion.
