#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------

SAFE_RUN() {
    local SF_RETURN_VALUE=$($1 2>&1)

    if [[ $? != 0 ]]; then
        >&2 echo $SF_RETURN_VALUE
        exit 1
    fi
    echo $SF_RETURN_VALUE
}

PRINT_USAGE() {
    echo ""
    echo "[ChakraCore Build Script Help]"
    echo ""
    echo "build.sh [options]"
    echo ""
    echo "options:"
    echo "      --cxx=PATH      Path to Clang++ (see example below)"
    echo "      --cc=PATH       Path to Clang   (see example below)"
    echo "  -d, --debug         Debug build (by default Release build)"
    echo "  -h, --help          Show help"
    echo "      --icu=PATH      Path to ICU include folder (see example below)"
    echo "  -j [N], --jobs[=N]  Multicore build, allow N jobs at once"
    echo "  -n, --ninja         Build with ninja instead of make"
    echo "  -t, --test-build    Test build (by default Release build)"
    echo "  -v, --verbose       Display verbose output including all options"
    echo ""
    echo "example:"
    echo "  ./build.sh --cxx=/path/to/clang++ --cc=/path/to/clang -j"
    echo "with icu:"
    echo "  ./build.sh --icu=/usr/local/Cellar/icu4c/version/include/"
    echo ""
}

CHAKRACORE_DIR=`dirname $0`
_CXX=""
_CC=""
VERBOSE=""
BUILD_TYPE="Release"
CMAKE_GEN=
MAKE=make
MULTICORE_BUILD=""
ICU_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
    --cxx=*)
        _CXX=$1
        _CXX=${_CXX:6}
        ;;

    --cc=*)
        _CC=$1
        _CC=${_CC:5}
        ;;

    -h | --help)
        PRINT_USAGE
        exit
        ;;

    -v | --verbose)
        _VERBOSE="verbose"
        ;;

    -d | --debug)
        BUILD_TYPE="Debug"
        ;;

    -t | --test-build)
        BUILD_TYPE="Test"
        ;;

    -j | --jobs)
        if [[ "$1" == "-j" && "$2" =~ ^[^-] ]]; then
            MULTICORE_BUILD="-j $2"
            shift
        else
            MULTICORE_BUILD="-j $(nproc)"
        fi
        ;;

    -j=* | --jobs=*)            # -j=N syntax used in CI
        MULTICORE_BUILD=$1
        if [[ "$1" =~ ^-j= ]]; then
            MULTICORE_BUILD="-j ${MULTICORE_BUILD:3}"
        else
            MULTICORE_BUILD="-j ${MULTICORE_BUILD:7}"
        fi
        ;;

    --icu=*)
        ICU_PATH=$1
        ICU_PATH="-DICU_INCLUDE_PATH=${ICU_PATH:6}"
        ;;

    -n | --ninja)
        CMAKE_GEN="-G Ninja"
        MAKE=ninja
        ;;

    *)
        echo "Unknown option $1"
        PRINT_USAGE
        exit -1
        ;;
    esac

    shift
done

if [[ ${#_VERBOSE} > 0 ]]; then
    # echo options back to the user
    echo "Printing command line options back to the user:"
    echo "_CXX=${_CXX}"
    echo "_CC=${_CC}"
    echo "BUILD_TYPE=${BUILD_TYPE}"
    echo "MULTICORE_BUILD=${MULTICORE_BUILD}"
    echo "ICU_PATH=${ICU_PATH}"
    echo "CMAKE_GEN=${CMAKE_GEN}"
    echo "MAKE=${MAKE}"
    echo ""
fi

CLANG_PATH=
if [[ ${#_CXX} > 0 || ${#_CC} > 0 ]]; then
    if [[ ${#_CXX} == 0 || ${#_CC} == 0 ]]; then
        echo "ERROR: '-cxx' and '-cc' options must be used together."
        exit 1
    fi
    echo "Custom CXX ${_CXX}"
    echo "Custom CC  ${_CC}"

    if [[ ! -f $_CXX || ! -f $_CC ]]; then
        echo "ERROR: Custom compiler not found on given path"
        exit 1
    fi
    CLANG_PATH=$_CXX
else
    RET_VAL=$(SAFE_RUN 'c++ --version')
    if [[ ! $RET_VAL =~ "clang" ]]; then
        echo "Searching for Clang..."
        if [[ -f /usr/bin/clang++ ]]; then
            echo "Clang++ found at /usr/bin/clang++"
            _CXX=/usr/bin/clang++
            _CC=/usr/bin/clang
            CLANG_PATH=$_CXX
        else
            echo "ERROR: clang++ not found at /usr/bin/clang++"
            echo ""
            echo "You could use clang++ from a custom location."
            PRINT_USAGE
            exit 1
        fi
    else
        CLANG_PATH=c++
    fi
fi

# check clang version (min required 3.7)
VERSION=$($CLANG_PATH --version | grep "version [0-9]*\.[0-9]*" --o -i | grep "[0-9]\.[0-9]*" --o)
VERSION=${VERSION/./}

if [[ $VERSION -lt 37 ]]; then
    echo "ERROR: Minimum required Clang version is 3.7"
    exit 1
fi

CC_PREFIX=""
if [[ ${#_CXX} > 0 ]]; then
    CC_PREFIX="-DCMAKE_CXX_COMPILER=$_CXX -DCMAKE_C_COMPILER=$_CC"
fi

build_directory="$CHAKRACORE_DIR/BuildLinux/${BUILD_TYPE:0}"
if [ ! -d "$build_directory" ]; then
    SAFE_RUN `mkdir -p $build_directory`
fi

pushd $build_directory > /dev/null

echo Generating $BUILD_TYPE makefiles
cmake $CMAKE_GEN $CC_PREFIX $ICU_PATH -DCMAKE_BUILD_TYPE=$BUILD_TYPE ../..

$MAKE $MULTICORE_BUILD 2>&1 | tee build.log
popd > /dev/null
