Node.js on ChakraCore
===
This project enables Node.js to optionally use the ChakraCore JavaScript engine. This project is still **work in progress** and not an officially supported Node.js branch. Please refer to the original [PR](https://github.com/nodejs/node/pull/4765).

### How it works

To enable building and running Node.js with the ChakraCore JavaScript engine, a V8 API shim (ChakraShim) is created on top of the ChakraCore runtime hosting API ([JSRT]
(https://msdn.microsoft.com/en-us/library/dn249673(v=vs.94).aspx)). ChakraShim implements the most essential V8 APIs so that the underlying JavaScript engine change is transparent to Node.js and other native addon modules written for V8.

A rebuild node.exe and native addon modules with ChakraCore is required for this to work.

### Issues
Please report all issues related to Node-ChakraCore on this separate [issue page](https://github.com/nodejs/node-chakracore/issues).

### Windows

#### Prebuilt Node binaries
You can download and install prebuilt Node-ChakraCore from [here](https://github.com/nodejs/node-chakracore/releases).

#### How to build
If you are looking to build this yourself. Here's what you will need.

Prerequisites:
* Windows 7 SP1 or higher (Windows 8.1 or higher for ARM builds)
* [Python 2.6 or 2.7](https://www.python.org)
* [Visual
Studio](https://www.visualstudio.com/en-us/downloads/download-visual-studio-vs.aspx)

Build Command:
```batch
vcbuild chakracore nosign [x86|x64|arm]
```

#### How to test

```batch
vcbuild chakracore nobuild test [x86|x64|arm]
```

To test if Node.js was built correctly with ChakraCore:

```batch
C:\>node -e "console.log('Hello from Node.js ' + process.jsEngine)"
Hello from Node.js chakracore
```

### Linux

NOTE: This is work in progress and contents are subject to change.

The build and test steps are similar to official node.js, with a few
customizations to build and run with chakracore.

#### How to build

Prerequisites:
* Ubuntu 16.04 LTS x64

Install building tools and ChakraCore dependencies:

```
sudo apt-get install -y build-essential cmake clang uuid-dev libunwind-dev libicu-dev liblttng-ust-dev
```

Clone this repo and check out `xplat` branch:

```
git checkout xplat
```

Build:

```
./configure --engine=chakracore
make
[sudo] make install
```

#### How to test

To test if Node.js was built correctly with ChakraCore:

```
$ node -e "console.log('Hello from Node.js ' + process.jsEngine)"
Hello from Node.js chakracore
```

To run unit tests:

```
$ make test
```
