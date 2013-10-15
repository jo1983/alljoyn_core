# Copyright 2010 - 2013, Qualcomm Innovation Center, Inc.
# 
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
# 
#        http://www.apache.org/licenses/LICENSE-2.0
# 
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
# 

import os
Import('env')

# Indicate that this SConscript file has been loaded already
env['_ALLJOYNCORE_'] = True

# Header files in common require "dist/cpp/inc/alljoyn" in the include path to find Status.h
env.Append(CPPPATH = ['$DISTDIR/cpp/inc/alljoyn'])

# Dependent Projects
common_hdrs, common_static_objs, common_shared_objs = env.SConscript(['../common/SConscript'])


# Make alljoyn C++ dist a sub-directory of the alljoyn dist.
env['CPP_DISTDIR'] = env['DISTDIR'] + '/cpp'
env['CPP_TESTDIR'] = env['TESTDIR'] + '/cpp'
env['WINRT_DISTDIR'] = env['DISTDIR'] + '/winRT'

ajenv = env.Clone()

# Bullseye code coverage for 'debug' builds.
if ajenv['VARIANT'] == 'debug':
    if(not(ajenv.has_key('BULLSEYE_BIN'))):
        print('BULLSEYE_BIN not specified')
    else:
        ajenv.PrependENVPath('PATH', ajenv.get('BULLSEYE_BIN'))
        if (not(os.environ.has_key('COVFILE'))):
            print('Error: COVFILE environment variable must be set')
            if not GetOption('help'):
                Exit(1)
        else:
            ajenv.PrependENVPath('COVFILE', os.environ['COVFILE'])

# manually add dependencies for xml to h, and for files included in the xml
ajenv.Depends('$OBJDIR/Status.h', 'src/Status.xml')
ajenv.Depends('$OBJDIR/Status.h', '../common/src/Status.xml')
ajenv.Append(STATUS_FLAGS=['--base=%s' % os.getcwd()])


if ajenv['OS_GROUP'] == 'winrt':
    ajenv.Depends('$OBJDIR/Status_CPP0x.h', 'src/Status.xml')
    ajenv.Depends('$OBJDIR/Status_CPP0x.h', '../common/src/Status.xml')
    ajenv.AppendUnique(CFLAGS=['/D_WINRT_DLL'])
    ajenv.AppendUnique(CXXFLAGS=['/D_WINRT_DLL'])
    ajenv.Append(STATUS_FLAGS=['--cpp0xnamespace=AllJoyn'])

# Add support for multiple build targets in the same workset
ajenv.VariantDir('$OBJDIR', 'src', duplicate = 0)
ajenv.VariantDir('$OBJDIR/test', 'test', duplicate = 0)
ajenv.VariantDir('$OBJDIR/daemon', 'daemon', duplicate = 0)
ajenv.VariantDir('$OBJDIR/samples', 'samples', duplicate = 0)
ajenv.VariantDir('$OBJDIR/alljoyn_android', 'alljoyn_android', duplicate = 0)

# AllJoyn Install
ajenv.Install('$OBJDIR', ajenv.File('src/Status.xml'))
ajenv.Status('$OBJDIR/Status')
core_headers = ajenv.Install('$CPP_DISTDIR/inc/alljoyn', '$OBJDIR/Status.h')
if ajenv['OS_GROUP'] == 'winrt':
    core_headers += ajenv.Install('$CPP_DISTDIR/inc/alljoyn', '$OBJDIR/Status_CPP0x.h')

core_headers += ajenv.Install('$CPP_DISTDIR/inc/alljoyn', [ h for h in ajenv.Glob('inc/alljoyn/*.h') if h not in ajenv.Glob('inc/alljoyn/Status*.h') ])

for d, h in common_hdrs.items():
    core_headers += ajenv.Install('$CPP_DISTDIR/inc/%s' % d, h)

# Header file includes
#ajenv.Append(CPPPATH = [ajenv.Dir('$CPP_DISTDIR/inc'), ajenv.Dir('$CPP_DISTDIR/inc/alljoyn')])

# Make private headers available
ajenv.Append(CPPPATH = [ajenv.Dir('src')])

# AllJoyn Libraries
libs, static_objs, shared_objs = ajenv.SConscript('$OBJDIR/SConscript', exports = ['ajenv', 'common_static_objs', 'common_shared_objs'])

ajenv.Install('$CPP_DISTDIR/lib', libs)

if ajenv['OS_GROUP'] != 'winrt':
    # Do not include alljoyn.lib in LIBS otherwise linking errors will occur.
    env.Prepend(LIBS = 'alljoyn')

# AllJoyn Daemon, daemon library, and bundled daemon object file
if ajenv['OS_GROUP'] == 'winrt':
    winrt_objs = static_objs
    daemon_progs, bdlib, bdobj = ajenv.SConscript('$OBJDIR/daemon/SConscript', exports = ['winrt_objs'])
    ajenv.Install('$WINRT_DISTDIR/bin', daemon_progs)
    # WinRT needs the full path to the exact file.
    env.Prepend(LIBS = [bdobj, bdlib])
    daemon_obj = [bdobj]

else:
    daemon_progs, bdlib, bdobj = ajenv.SConscript('$OBJDIR/daemon/SConscript')
    ajenv.Install('$CPP_DISTDIR/bin', daemon_progs)
    ajenv.Install('$CPP_DISTDIR/lib', bdlib)
    daemon_obj = ajenv.Install('$CPP_DISTDIR/lib', bdobj)
    # Need to prepend rather than append to ensure proper static library ordering
    if env['BD'] == 'on':
        env.Prepend(LIBS = [bdobj, 'ajdaemon'])


# The global env needs the 'bdobj' for the Java binding
env['bdobj'] = daemon_obj

# only include command line samples, unit test, test programs if we are not 
# building for iOS. No support on iOS for command line applications.
if env['OS'] == 'darwin' and env['CPU'] in ['arm', 'armv7', 'armv7s']:
    progs = []
else:
    # Test programs
    progs = env.SConscript('$OBJDIR/test/SConscript')
    ajenv.Install('$CPP_DISTDIR/bin', progs)

    # Build unit Tests
    env.SConscript('unit_test/SConscript', variant_dir='$OBJDIR/unittest', duplicate = 0)

    # Sample programs
    env.SConscript('$OBJDIR/samples/SConscript')

# Android daemon runner
ajenv.SConscript('$OBJDIR/alljoyn_android/SConscript')

# Release notes and misc. legals
if ajenv['OS_CONF'] == 'darwin':
    if ajenv['CPU'] == 'x86':
        ajenv.InstallAs('$DISTDIR/README.txt', 'docs/README.darwin.txt')
elif ajenv['OS_CONF'] == 'winrt':
    ajenv.InstallAs('$DISTDIR/README.txt', 'docs/README.winrt.txt')
elif ajenv['OS_CONF'] == 'windows':
    ajenv.InstallAs('$DISTDIR/README.txt', 'docs/README.windows.txt')
    ajenv.Install('$DISTDIR', 'docs/AllJoyn_API_Changes_java.txt')
elif ajenv['OS_CONF'] == 'android':
    ajenv.InstallAs('$DISTDIR/README.txt', 'docs/README.android.txt')
    ajenv.Install('$DISTDIR', 'docs/AllJoyn_API_Changes_java.txt')
else:  # linux based platforms
    ajenv.InstallAs('$DISTDIR/README.txt', 'docs/README.linux.txt')
    ajenv.Install('$DISTDIR', 'docs/AllJoyn_API_Changes_java.txt')

if not (ajenv['OS'] == 'darwin' and ajenv['CPU'] in ['arm', 'armv7', 'armv7s']):
    ajenv.Install('$DISTDIR', 'docs/AllJoyn_API_Changes_cpp.txt')
    ajenv.Install('$DISTDIR', 'docs/ReleaseNotes.txt')
    ajenv.Install('$DISTDIR', 'README.md')
    ajenv.Install('$DISTDIR', 'NOTICE.txt')

# Build docs
installDocs = ajenv.SConscript('docs/SConscript', exports = ['ajenv', 'core_headers'])

#Build Win8 SDK installer
ajenv.SConscript('win8_sdk/SConscript', exports = ['ajenv'])
