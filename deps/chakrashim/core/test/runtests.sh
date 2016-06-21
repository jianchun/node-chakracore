#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
#
# todo-CI: REMOVE THIS AFTER ENABLING runtests.py directly on CI

test_path=`dirname "$0"`

build_type=$1
# Accept -d or -t. If none was given (i.e. current CI), 
# search for the known paths
if [[ $build_type != "-d" && $build_type != "-t" ]]; then
    echo "Warning: You haven't provide either '-d' (debug) or '-t' (test)."
    echo "Warning: Searching for ch.."
    if [[ -f "$test_path/../BuildLinux/Debug/ch" ]]; then
        echo "Warning: Debug build was found"
        build_type="-d"
    elif [[ -f "$test_path/../BuildLinux/Test/ch" ]]; then
        echo "Warning: Test build was found"
        build_type="-t"
    else
        echo 'Error: ch not found- exiting'
        exit 1
    fi
fi

"$test_path/runtests.py" $build_type
