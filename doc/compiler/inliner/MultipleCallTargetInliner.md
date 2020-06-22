<!--
Copyright (c) 2000, 2019 IBM Corp. and others

This program and the accompanying materials are made available under
the terms of the Eclipse Public License 2.0 which accompanies this
distribution and is available at https://www.eclipse.org/legal/epl-2.0/
or the Apache License, Version 2.0 which accompanies this distribution and
is available at https://www.apache.org/licenses/LICENSE-2.0.

This Source Code may also be made available under the following
Secondary Licenses when the conditions for such availability set
forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
General Public License, version 2 with the GNU Classpath
Exception [1] and GNU General Public License, version 2 with the
OpenJDK Assembly Exception [2].

[1] https://www.gnu.org/software/classpath/license.html
[2] http://openjdk.java.net/legal/assembly-exception.html

SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
-->
# Overview

**TR_MultipleTargetInliner** is a subclass of **TR_InlinerBase**. Compared
to its sibling **TR_DumbInliner**, **TR_MultipleTargetInliner** encapsulate
more complicated heuristic to decide whether a method or a sub callgraph
should be inlined. Also, as its name suggested, it has the ability to inline
multiple calltargets for one call site. This document explains the overall
code flow at a high level and some more specific compoments in more details.
This document focus on things specific to ***TR_MultipleTargetInliner* and
for more general concepts applies to all the inliner classes please refer
to omr/inliner.

# Code Flow
At a high level, **TR_MultipleTargetInliner** looks for call sites to inline
recursivelly. Once a call target is selected
to be inlined for a call site inliner might decide to further inline call sites in this call
target and so on and so forth. The proceses eventually forms a call graph with
a call callsite pointing to its calltargets and the call target points to all
its callsites.
```cpp


            depth 0                            depth 1
            callsites                          callsites

                                             +->callsite 0.0.0
                                             |
                          +->calltarget 0.0  +->callsite 0.0.1
                          |                  |
           +->callsite 0  +->calltarget 0.1  +->...
           |              |
           |              +->...
 compiling |
  method   |              +->calltarget 1.0
           |              |                  ...
           +->callsite 1  +->calltarget 2.1
           |              |
           |              +->...
           |
           |
           +->...
```

## call sites at depth 0

The top level call sites are found by InlineCalltargets looking for call
nodes from IL trees of method being compiled. Whenever a callNode is encountered
while iterating IL trees, a call site is created and `weighCallSite` is
called on that callSite to trigger:
1. the recursion process to discover the call graph rooted in this call site
2. the heuristics on deciding which parts in the call graph is worth inlining.

## call sites with more depth

The the process of recursively looking for second level call sites is done
through a class called TR_J9EstimateCodeSize. **estimateCodeSize** is called on
a candidate call target to do byte code iteration on its Java method body to
look for callsites in this target. Besides discovering
call sites, it also compare the target's
byte codes count and against certain threshoulds in order to decide whether the call target would
be inlined. The reason we say it's an **estimate** is because that byte codes count
doesn't equal to the accurate size which is the number of tree nodes that's going  to be generated for this
method when it's actually inlined.

For each call target the function is called twice. The first time it's called,
only thresholds related to a single method would be checked. If the single method inlining
conditions are satisfied, **estimateCodeSize** would be called on the same target for the second time with recursion
to collect the sub call graph. Threshoulds applying for a sub call graph would then
be checked against the accumulative code size for the sub call graph.

# method handle chain inlining
