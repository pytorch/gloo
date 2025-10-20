#!/usr/bin/env python3
""" The Python Hipify script.
##
# Copyright (c) 2015-2016 Advanced Micro Devices, Inc. All rights reserved.
#               2017-2018 Advanced Micro Devices, Inc. and
#                         Facebook Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
"""


import fnmatch
import os
import re

from pyHIPIFY.cuda_to_hip_mappings import CUDA_TO_HIP_MAPPINGS


class InputError(Exception):
    # Exception raised for errors in the input.

    def __init__(self, message):
        super().__init__(message)
        self.message = message

    def __str__(self):
        return "{}: {}".format("Input error", self.message)


def matched_files_iter(root_path, includes=("*",), ignores=(), extensions=()):
    def _fnmatch(filepath, patterns):
        return any(fnmatch.fnmatch(filepath, pattern) for pattern in patterns)

    def match_extensions(filename):
        """Helper method to see if filename ends with certain extension"""
        return any(filename.endswith(e) for e in extensions)

    exact_matches = set(includes)

    # This is a very rough heuristic; really, we want to avoid scanning
    # any file which is not checked into source control, but this script
    # needs to work even if you're in a Git or Hg checkout, so easier to
    # just blacklist the biggest time sinks that won't matter in the
    # end.
    for abs_dirpath, dirs, filenames in os.walk(root_path, topdown=True):
        rel_dirpath = os.path.relpath(abs_dirpath, root_path)
        if rel_dirpath == ".":
            # Blah blah blah O(n) blah blah
            if ".git" in dirs:
                dirs.remove(".git")
            if "build" in dirs:
                dirs.remove("build")
            if "third_party" in dirs:
                dirs.remove("third_party")
        for filename in filenames:
            filepath = os.path.join(rel_dirpath, filename)
            # We respect extensions, UNLESS you wrote the entire
            # filename verbatim, in which case we always accept it
            if (
                _fnmatch(filepath, includes)
                and (not _fnmatch(filepath, ignores))
                and (match_extensions(filepath) or filepath in exact_matches)
            ):
                yield filepath


def preprocess(project_directory, output_directory, all_files, show_progress=True):
    """
    Call preprocessor on selected files.
    """

    for filepath in all_files:
        preprocessor(project_directory, output_directory, filepath)
        if show_progress:
            print(filepath, "->", get_hip_file_path(filepath))

    print("Successfully preprocessed all matching files.")


def add_dim3(kernel_string, cuda_kernel):
    """adds dim3() to the second and third arguments in the kernel launch"""
    count = 0
    closure = 0
    kernel_string = kernel_string.replace("<<<", "").replace(">>>", "")
    arg_locs = [{} for _ in range(2)]
    arg_locs[count]["start"] = 0
    for ind, c in enumerate(kernel_string):
        if count > 1:
            break
        if c == "(":
            closure += 1
        elif c == ")":
            closure -= 1
        elif (c == "," or ind == len(kernel_string) - 1) and closure == 0:
            arg_locs[count]["end"] = ind + (c != ",")
            count += 1
            if count < 2:
                arg_locs[count]["start"] = ind + 1

    first_arg_raw = kernel_string[arg_locs[0]["start"] : arg_locs[0]["end"] + 1]
    second_arg_raw = kernel_string[arg_locs[1]["start"] : arg_locs[1]["end"]]

    first_arg_clean = (
        kernel_string[arg_locs[0]["start"] : arg_locs[0]["end"]]
        .replace("\n", "")
        .strip(" ")
    )
    second_arg_clean = (
        kernel_string[arg_locs[1]["start"] : arg_locs[1]["end"]]
        .replace("\n", "")
        .strip(" ")
    )

    first_arg_dim3 = f"dim3({first_arg_clean})"
    second_arg_dim3 = f"dim3({second_arg_clean})"

    first_arg_raw_dim3 = first_arg_raw.replace(first_arg_clean, first_arg_dim3)
    second_arg_raw_dim3 = second_arg_raw.replace(second_arg_clean, second_arg_dim3)
    cuda_kernel = cuda_kernel.replace(
        first_arg_raw + second_arg_raw, first_arg_raw_dim3 + second_arg_raw_dim3
    )
    return cuda_kernel


RE_KERNEL_LAUNCH = re.compile(r"([ ]+)(detail?)::[ ]+\\\n[ ]+")


def processKernelLaunches(string):
    """Replace the CUDA style Kernel launches with the HIP style kernel launches."""
    # Concat the namespace with the kernel names. (Find cleaner way of doing this later).
    string = RE_KERNEL_LAUNCH.sub(lambda inp: f"{inp.group(1)}{inp.group(2)}::", string)

    def grab_method_and_template(in_kernel):
        # The positions for relevant kernel components.
        pos = {
            "kernel_launch": {"start": in_kernel["start"], "end": in_kernel["end"]},
            "kernel_name": {"start": -1, "end": -1},
            "template": {"start": -1, "end": -1},
        }

        # Count for balancing template
        count = {"<>": 0}

        # Status for whether we are parsing a certain item.
        START = 0
        AT_TEMPLATE = 1
        AFTER_TEMPLATE = 2
        AT_KERNEL_NAME = 3

        status = START

        # Parse the string character by character
        for i in range(pos["kernel_launch"]["start"] - 1, -1, -1):
            char = string[i]

            # Handle Templating Arguments
            if status == START or status == AT_TEMPLATE:
                if char == ">":
                    if status == START:
                        status = AT_TEMPLATE
                        pos["template"]["end"] = i
                    count["<>"] += 1

                if char == "<":
                    count["<>"] -= 1
                    if count["<>"] == 0 and (status == AT_TEMPLATE):
                        pos["template"]["start"] = i
                        status = AFTER_TEMPLATE

            # Handle Kernel Name
            if status != AT_TEMPLATE:
                if string[i].isalnum() or string[i] in {"(", ")", "_", ":", "#"}:
                    if status != AT_KERNEL_NAME:
                        status = AT_KERNEL_NAME
                        pos["kernel_name"]["end"] = i

                    # Case: Kernel name starts the string.
                    if i == 0:
                        pos["kernel_name"]["start"] = 0

                        # Finished
                        return [
                            (pos["kernel_name"]),
                            (pos["template"]),
                            (pos["kernel_launch"]),
                        ]

                else:
                    # Potential ending point if we're already traversing a kernel's name.
                    if status == AT_KERNEL_NAME:
                        pos["kernel_name"]["start"] = i

                        # Finished
                        return [
                            (pos["kernel_name"]),
                            (pos["template"]),
                            (pos["kernel_launch"]),
                        ]

    def find_kernel_bounds(string):
        """Finds the starting and ending points for all kernel launches in the string."""
        kernel_end = 0
        kernel_positions = []

        # Continue until we cannot find any more kernels anymore.
        while string.find("<<<", kernel_end) != -1:
            # Get kernel starting position (starting from the previous ending point)
            kernel_start = string.find("<<<", kernel_end)

            # Get kernel ending position (adjust end point past the >>>)
            kernel_end = string.find(">>>", kernel_start) + 3
            if kernel_end <= 0:
                raise InputError("no kernel end found")

            # Add to list of traversed kernels
            kernel_positions.append(
                {
                    "start": kernel_start,
                    "end": kernel_end,
                    "group": string[kernel_start:kernel_end],
                }
            )

        return kernel_positions

    # Grab positional ranges of all kernel launchces
    get_kernel_positions = [k for k in find_kernel_bounds(string)]
    output_string = string

    # Replace each CUDA kernel with a HIP kernel.
    for kernel in get_kernel_positions:
        # Get kernel components
        params = grab_method_and_template(kernel)

        # Find parenthesis after kernel launch
        parenthesis = string.find("(", kernel["end"])

        # Extract cuda kernel
        cuda_kernel = string[params[0]["start"] : parenthesis + 1]
        kernel_string = string[kernel["start"] : kernel["end"]]
        cuda_kernel_dim3 = add_dim3(kernel_string, cuda_kernel)
        # Keep number of kernel launch params consistent (grid dims, group dims, stream, dynamic shared size)
        num_klp = len(
            extract_arguments(
                0, kernel["group"].replace("<<<", "(").replace(">>>", ")")
            )
        )

        hip_kernel = "hipLaunchKernelGGL(" + cuda_kernel_dim3[0:-1].replace(
            ">>>", ", 0" * (4 - num_klp) + ">>>"
        ).replace("<<<", ", ").replace(">>>", ", ")

        # Replace cuda kernel with hip kernel
        output_string = output_string.replace(cuda_kernel, hip_kernel)

    return output_string


def get_hip_file_path(filepath):
    """
    Returns the new name of the hipified file
    """
    dirpath, filename = os.path.split(filepath)
    root, ext = os.path.splitext(filename)

    # Concretely, we do the following:
    #
    #   - If there is a directory component named "cuda", replace
    #     it with "hip", AND
    #
    #   - If the file name contains "CUDA", replace it with "HIP", AND
    # Furthermore, ALWAYS replace '.cu' with '.hip', because those files
    # contain CUDA kernels that needs to be hipified and processed with
    # hcc compiler
    #
    # This isn't set in stone; we might adjust this to support other
    # naming conventions.

    if ext == ".cu":
        ext = ".hip"

    orig_dirpath = dirpath
    dirpath = dirpath.replace("cuda", "hip")
    root = root.replace("cuda", "hip")
    root = root.replace("CUDA", "HIP")

    return os.path.join(dirpath, root + ext)


# Cribbed from https://stackoverflow.com/questions/42742810/speed-up-millions-of-regex-replacements-in-python-3/42789508#42789508
class Trie:
    """Regex::Trie in Python. Creates a Trie out of a list of words. The trie can be exported to a Regex pattern.
    The corresponding Regex should match much faster than a simple Regex union."""

    def __init__(self):
        self.data = {}

    def add(self, word):
        ref = self.data
        for char in word:
            ref[char] = char in ref and ref[char] or {}
            ref = ref[char]
        ref[""] = 1

    def dump(self):
        return self.data

    def quote(self, char):
        return re.escape(char)

    def _pattern(self, pData):
        data = pData
        if "" in data and len(data.keys()) == 1:
            return None

        alt = []
        cc = []
        q = 0
        for char in sorted(data.keys()):
            if isinstance(data[char], dict):
                try:
                    recurse = self._pattern(data[char])
                    alt.append(self.quote(char) + recurse)
                except Exception:
                    cc.append(self.quote(char))
            else:
                q = 1
        cconly = not len(alt) > 0

        if len(cc) > 0:
            if len(cc) == 1:
                alt.append(cc[0])
            else:
                alt.append("[" + "".join(cc) + "]")

        if len(alt) == 1:
            result = alt[0]
        else:
            result = "(?:" + "|".join(alt) + ")"

        if q:
            if cconly:
                result += "?"
            else:
                result = "(?:%s)?" % result
        return result

    def pattern(self):
        return self._pattern(self.dump())


RE_TRIE = Trie()
RE_MAP = {}
for mapping in CUDA_TO_HIP_MAPPINGS:
    for src, value in mapping.items():
        dst = value[0]
        RE_TRIE.add(src)
        RE_MAP[src] = dst
RE_PREPROCESSOR = re.compile(RE_TRIE.pattern())


def re_replace(input_string):
    def sub_repl(m):
        return RE_MAP[m.group(0)]

    return RE_PREPROCESSOR.sub(sub_repl, input_string)


def preprocessor(project_directory, output_directory, filepath):
    """Executes the CUDA -> HIP conversion on the specified file."""
    fin_path = os.path.join(project_directory, filepath)
    with open(fin_path) as fin:
        output_source = fin.read()

    fout_path = os.path.join(output_directory, get_hip_file_path(filepath))
    assert os.path.join(output_directory, fout_path) != os.path.join(
        project_directory, fin_path
    )
    if not os.path.exists(os.path.dirname(fout_path)):
        os.makedirs(os.path.dirname(fout_path))

    with open(fout_path, "w") as fout:
        output_source = re_replace(output_source)

        # Perform Kernel Launch Replacements
        output_source = processKernelLaunches(output_source)

        fout.write(output_source)


def extract_arguments(start, string):
    """Return the list of arguments in the upcoming function parameter closure.
    Example:
    string (input): '(blocks, threads, 0, THCState_getCurrentStream(state))'
    arguments (output):
        '[{'start': 1, 'end': 7},
        {'start': 8, 'end': 16},
        {'start': 17, 'end': 19},
        {'start': 20, 'end': 53}]'
    """

    arguments = []
    closures = {"<": 0, "(": 0}
    current_position = start
    argument_start_pos = current_position + 1

    # Search for final parenthesis
    while current_position < len(string):
        if string[current_position] == "(":
            closures["("] += 1
        elif string[current_position] == ")":
            closures["("] -= 1
        elif string[current_position] == "<":
            closures["<"] += 1
        elif (
            string[current_position] == ">"
            and string[current_position - 1] != "-"
            and closures["<"] > 0
        ):
            closures["<"] -= 1

        # Finished all arguments
        if closures["("] == 0 and closures["<"] == 0:
            # Add final argument
            arguments.append({"start": argument_start_pos, "end": current_position})
            break

        # Finished current argument
        if (
            closures["("] == 1
            and closures["<"] == 0
            and string[current_position] == ","
        ):
            arguments.append({"start": argument_start_pos, "end": current_position})
            argument_start_pos = current_position + 1

        current_position += 1

    return arguments


def hipify(
    project_directory,
    extensions=(".cu", ".cuh", ".c", ".cc", ".cpp", ".h", ".in", ".hpp"),
    output_directory=None,
    includes=(),
    ignores=(),
    list_files_only=False,
    show_progress=True,
):
    assert os.path.exists(project_directory)

    # If no output directory, provide a default one.
    if not output_directory:
        output_directory = os.path.join(project_directory, "hip")

    all_files = list(
        matched_files_iter(
            project_directory, includes=includes, ignores=ignores, extensions=extensions
        )
    )
    if list_files_only:
        print(os.linesep.join(all_files))
        return

    # Start Preprocessor
    preprocess(
        project_directory, output_directory, all_files, show_progress=show_progress
    )
