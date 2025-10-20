#!/usr/bin/env python3


import argparse
import os

from pyHIPIFY import hipify_python

parser = argparse.ArgumentParser(
    description="Top-level script for HIPifying, filling in most common parameters"
)
parser.add_argument(
    "--project-directory",
    type=str,
    default=os.path.normpath(
        os.path.join(
            os.path.realpath(__file__),
            os.pardir,
            os.pardir,
            os.pardir,
        )
    ),
    help="The root of the project. (default: %(default)s)",
    required=False,
)

parser.add_argument(
    "--output-directory",
    type=str,
    default="",
    help="The Directory to Store the Hipified Project",
    required=False,
)

parser.add_argument(
    "--list-files-only",
    action="store_true",
    help="Only print the list of hipify files.",
)

parser.add_argument(
    "--root-dir",
    type=str,
    default="gloo",
    help="The root directory of gloo project",
    required=False,
)


args = parser.parse_args()

amd_build_dir = os.path.dirname(os.path.realpath(__file__))
proj_dir = os.path.join(os.path.dirname(os.path.dirname(amd_build_dir)))

if args.project_directory:
    proj_dir = args.project_directory

out_dir = proj_dir
if args.output_directory:
    out_dir = args.output_directory

includes = [
    os.path.join(args.root_dir, "*cuda*"),
    os.path.join(args.root_dir, "*nccl*"),
]

ignores = []

hipify_python.hipify(
    project_directory=proj_dir,
    output_directory=out_dir,
    includes=includes,
    ignores=ignores,
    list_files_only=args.list_files_only,
    show_progress=False,
)
