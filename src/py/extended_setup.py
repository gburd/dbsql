# ext_setup.py: setuptools extensions for the distutils script
#
# DBSQL - A SQL database engine.
#
# Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
#
# This library is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# There are special exceptions to the terms and conditions of the GPL as it
# is applied to this software. View the full text of the exception in file
# LICENSE_EXCEPTIONS in the directory of this software distribution.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# Copyright (C) 2004-2006 Gerhard Häring <gh@ghaering.de>
#
# This software is provided 'as-is', without any express or implied
# warranty.  In no event will the authors be held liable for any damages
# arising from the use of this software.
#
# Permission is granted to anyone to use this software for any purpose,
# including commercial applications, and to alter it and redistribute it
# freely, subject to the following restrictions:
#
# 1. The origin of this software must not be misrepresented; you must not
#    claim that you wrote the original software. If you use this software
#    in a product, an acknowledgment in the product documentation would be
#    appreciated but is not required.
# 2. Altered source versions must be plainly marked as such, and must not be
#    misrepresented as being the original software.
# 3. This notice may not be removed or altered from any source distribution.

import glob, os, sys

from ez_setup import use_setuptools
use_setuptools()

import setuptools
import setup

class DocBuilder(setuptools.Command):
    description = "Builds the documentation"
    user_options = []

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):
        import os, stat

        try:
            import docutils.core
            import docutilsupport
        except ImportError:
            print "Error: the build-docs command requires docutils and SilverCity to be installed"
            return

        docfiles = {
            "usage-guide.html": "usage-guide.txt",
            "install-source.html": "install-source.txt",
            "install-source-win32.html": "install-source-win32.txt"
        }

        os.chdir("doc")
        for dest, source in docfiles.items():
            if not os.path.exists(dest) or os.stat(dest)[stat.ST_MTIME] < os.stat(source)[stat.ST_MTIME]:
                print "Building documentation file %s." % dest
                docutils.core.publish_cmdline(
                    writer_name='html',
                    argv=["--stylesheet=default.css", source, dest])

        os.chdir("..")

def main():
    setup_args = setup.get_setup_args()
    setup_args["extras_require"] = {"build_docs": ["docutils", "SilverCity"]}
    setup_args["cmdclass"] = {"build_docs": DocBuilder}
    setup_args["test_suite"] = "dbsql.test.suite"
    setup_args["cmdclass"] = {"build_docs": DocBuilder}
    setup_args["extras_require"] = {"build_docs": ["docutils", "SilverCity"]}
    setuptools.setup(**setup_args)

if __name__ == "__main__":
    main()
