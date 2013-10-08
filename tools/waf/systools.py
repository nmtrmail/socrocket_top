#! /usr/bin/env python
# encoding: utf-8
#*********************************************************************
# Copyright 2010, Institute of Computer and Network Engineering,
#                 TU-Braunschweig
# All rights reserved
# Any reproduction, use, distribution or disclosure of this program,
# without the express, prior written consent of the authors is 
# strictly prohibited.
#
# University of Technology Braunschweig
# Institute of Computer and Network Engineering
# Hans-Sommer-Str. 66
# 38118 Braunschweig, Germany
#
# ESA SPECIAL LICENSE
#
# This program may be freely used, copied, modified, and redistributed
# by the European Space Agency for the Agency's own requirements.
#
# The program is provided "as is", there is no warranty that
# the program is correct or suitable for any purpose,
# neither implicit nor explicit. The program and the information in it
# contained do not necessarily reflect the policy of the European 
# Space Agency or of TU-Braunschweig.
#*********************************************************************
# Title:      modelsim.py
#
# ScssId:
#
# Origin:     HW-SW SystemC Co-Simulation SoC Validation Platform
#
# Purpose:    A build system integration for modelsim.
#
# Method:     $ ./waf configure; ./waf # to build it
#
# Modified on $Date: 2010-10-07 17:40:12 +0200 (Thu, 07 Oct 2010) $
#          at $Revision: 159 $
#          by $Author$
#
# Principal:  European Space Agency
# Author:     VLSI working group @ IDA @ TUBS
# Maintainer: Rolf Meyer
# Reviewed:
#*********************************************************************
from waflib import Task
from waflib import TaskGen
from waflib import Options
from waflib.Configure import conf
from waflib import Context
import Utils
from waflib import Utils,Task,Logs,Options
import os, sys

TESTLOCK = Utils.threading.Lock()

def options(opt):
    """Options for the Systool will be added here"""
    config = opt.get_option_group("--download")
    config.add_option('--nosystests', dest='systests', action='store_false', 
        default=True, help='Deactivates all tests executed on a platform')

def configure(ctx):
    """Systools configuration section"""
    ctx.env["SYSTESTS"] = Options.options.systests
    ctx.env["MKPROMFLAGS"] = []
    ctx.find_program('mkprom2', var='MKPROM', mandatory=True, okmsg="ok")

def systest_task_str(self):
    """Better Print Sting for Simulator Tests"""
    if hasattr(self,'rom'):
        return "utest: %s (%s, %s) on %s\n" \
            % (self.ram, self.rom, self.atstr, self.sys)
    else:
        return self.__oldstr__()

def systest_task_run(self):
    """Simulator Tests need a sligtly different setup"""
    if hasattr(self, 'rom'):
        filename = "%s (%s, %s) on %s" \
            % (self.filename, self.rom, self.atstr, self.sys)
    else:
        filename = self.inputs[0].abspath()

    self.ut_exec = getattr(self, 'ut_exec', [filename])

    if getattr(self.generator, 'ut_fun', None):
        self.generator.ut_fun(self)

    try:
        newenv = getattr(self.generator.bld, 'all_test_paths')

    except AttributeError:
        newenv = os.environ.copy()
        self.generator.bld.all_test_paths = newenv
        lst = []
        for group in self.generator.bld.groups:
            for task in group:
                if getattr(task, 'link_task', None):
                    lst.append(task.link_task.outputs[0].parent.abspath())

        def add_path(dct, path, var):
            """Add a path to a pathlist"""
            dct[var] = os.pathsep.join(Utils.to_list(path) + 
                [os.environ.get(var,'')])

        if sys.platform == 'win32':
            add_path(newenv, lst, 'PATH')

        elif sys.platform == 'darwin':
            add_path(newenv, lst, 'DYLD_LIBRARY_PATH')
            add_path(newenv, lst, 'LD_LIBRARY_PATH')

        else:
            add_path(newenv, lst, 'LD_LIBRARY_PATH')

    cwd = getattr(self.generator, 'ut_cwd', '') or \
        self.inputs[0].parent.abspath()
    proc = Utils.subprocess.Popen(self.ut_exec, cwd=cwd, env=newenv, 
        stderr=Utils.subprocess.PIPE, stdout=Utils.subprocess.PIPE)
    (stdout, stderr) = proc.communicate()
    result_tuple = (filename, proc.returncode, stdout, stderr)
    self.generator.utest_result = result_tuple
    TESTLOCK.acquire()

    try:
        bld = self.generator.bld
        Logs.debug("ut: %r", result_tuple)
        try:
            bld.utest_results.append(result_tuple)

        except AttributeError:
            bld.utest_results = [result_tuple]

    finally:
        TESTLOCK.release()

def make_systest(self):
    """Extended Testing support"""
    if not (Options.options.systests and self.env["SYSTESTS"]):
        return

    sysname = getattr(self, 'system', None)
    romname = getattr(self, 'prom', getattr(self, 'rom', None)) 
    sdramname = getattr(self, 'ram', getattr(self, 'sdram', None))
    sramname = getattr(self, 'sram', None)
    at_mode = getattr(self, 'at', False)
    atstr = "lt"
    atbool = "false"
    param = Utils.to_list( \
        getattr(self, 'args', \
            getattr(self, 'param', \
                getattr(self, 'ut_param', []))))
  
    if at_mode:
        atstr = "at"
        atbool = "true"

    if sysname and romname:
        systgen = self.bld.get_tgen_by_name(sysname)
        system = systgen.path.find_or_declare(sysname)

        romtgen = self.bld.get_tgen_by_name(romname)
        rom = romtgen.path.find_or_declare(romname)

        exec_list = [system.abspath(), "--option", 
            "conf.mctrl.prom.elf=%s" % (rom.abspath())]
        deps_list = [system, rom]
        filename = ""

        if sdramname:
            sdramtgen = self.bld.get_tgen_by_name(sdramname)
            sdram = sdramtgen.path.find_or_declare(sdramname)

            exec_list.append("--option")
            exec_list.append("conf.mctrl.ram.sdram.elf=%s" % (sdram.abspath()))
            exec_list.append("--option")
            exec_list.append("conf.system.osemu=%s" % (sdram.abspath()))
            exec_list.append("--option")
            exec_list.append("conf.system.log=%s-%s" % (sdram.abspath(), atstr))

            deps_list.append(sdram)

            filename = sdram.abspath()

        if sramname:
            sramtgen = self.bld.get_tgen_by_name(sramname)
            sram = sramtgen.path.find_or_declare(sramname)

            exec_list.append("--option")
            exec_list.append("conf.mctrl.ram.sram.elf=%s" % (sram.abspath()))
            exec_list.append("--option")
            exec_list.append("conf.system.osemu=%s" % (sram.abspath()))
            exec_list.append("--option")
            exec_list.append("conf.system.log=%s-%s" % (sram.abspath(), atstr))

            deps_list.append(sram)

            filename = sdram.abspath()

        exec_list.append("--option")
        exec_list.append("conf.system.at=%s" % (atbool))

        test = self.create_task('utest', deps_list)
        if not hasattr(test.__class__, '__oldstr__'):
            test.__class__.__oldstr__ = test.__class__.__str__
            test.__class__.__str__ = systest_task_str
            test.__class__.run = systest_task_run
        test.__unicode__ = systest_task_str
        test.sys = sysname
        test.ram = sdramname or sramname or ""
        test.rom = romname
        test.atstr = atstr
        test.filename = filename
        test.ut_exec = exec_list + param

from waflib.TaskGen import feature, after_method, before_method, task_gen
feature('systest')(make_systest)
after_method('apply_link')(make_systest)

MKPROM = Task.simple_task_type('MKPROM',
    '${MKPROM} ${_MKPROMFLAGS} -o ${TGT} ${SRC}',
    color='YELLOW')

MKPROM_TSKS = list()

def make_prom(self):
    """Create mkprom task"""
    elf = self.target
    if hasattr(self, 'promflags'):
        promflags = Utils.to_list(self.promflags)
    if hasattr(self, 'prom'):
        promname = self.prom
    elif elf.endswith(".sparc"):
        (head, sep, tail) = elf.rpartition(".sparc")
        promname = head + ".prom" + tail
    else:
        promname = elf + ".prom"

    taskgroup = self.bld(name=promname, target=promname)
    taskgroup.env['_MKPROMFLAGS'] = taskgroup.env['MKPROMFLAGS'] + promflags
    task = taskgroup.create_task('MKPROM', 
        src=[self.path.find_resource(elf)],
        tgt=[self.path.find_or_declare(promname)]
    )
    if MKPROM_TSKS and len(MKPROM_TSKS)>0:
        task.set_inputs(MKPROM_TSKS[-1].outputs[0])
    MKPROM_TSKS.append(task)
    taskgroup.post()

feature('prom')(make_prom)
after_method('apply_link')(make_prom)
before_method('make_systest')(make_prom)
