/*******************************************************************************
 * Copyright (c) 2000, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "compile/Compilation.hpp"
#if defined(J9VM_OPT_JITSERVER)
#include "control/CompilationRuntime.hpp"
#include "control/CompilationThread.hpp"
#include "control/JITServerHelpers.hpp"
#endif /* defined(J9VM_OPT_JITSERVER) */
#include "env/ClassEnv.hpp"
#include "env/CompilerEnv.hpp"
#include "env/jittypes.h"
#include "env/TypeLayout.hpp"
#include "env/VMJ9.h"
#include "j9.h"
#include "j9protos.h"
#include "j9cp.h"
#include "j9cfg.h"
#include "j9fieldsInfo.h"
#include "rommeth.h"
#include "runtime/RuntimeAssumptions.hpp"
#include "j9nonbuilder.h"
#include "env/j9method.h"
#include "il/SymbolReference.hpp"

class TR_PersistentClassInfo;
template <typename ListKind> class List;

/*  REQUIRES STATE (_vmThread).  MOVE vmThread to COMPILATION

TR_OpaqueClassBlock *
J9::ClassEnv::getClassFromJavaLangClass(uintptr_t objectPointer)
   {
   return (TR_OpaqueClassBlock*)J9VM_J9CLASS_FROM_HEAPCLASS(_vmThread, objectPointer);
   }
*/

TR::ClassEnv *
J9::ClassEnv::self()
   {
   return static_cast<TR::ClassEnv *>(this);
   }

J9Class *
J9::ClassEnv::convertClassOffsetToClassPtr(TR_OpaqueClassBlock *clazzOffset)
   {
   // NOTE : We could pass down vmThread() in the call below if the conversion
   // required the VM thread. Currently it does not. If we did change that
   // such that the VM thread was reqd, we would need to handle AOT where the
   // TR_FrontEnd is created with a NULL J9VMThread object.
   //
   return (J9Class*)((TR_OpaqueClassBlock *)clazzOffset);
   }

TR_OpaqueClassBlock *
J9::ClassEnv::convertClassPtrToClassOffset(J9Class *clazzPtr)
   {
   // NOTE : We could pass down vmThread() in the call below if the conversion
   // required the VM thread. Currently it does not. If we did change that
   // such that the VM thread was reqd, we would need to handle AOT where the
   // TR_FrontEnd is created with a NULL J9VMThread object.
   //
   return (TR_OpaqueClassBlock*)(clazzPtr);
   }

bool
J9::ClassEnv::isClassSpecialForStackAllocation(TR_OpaqueClassBlock * clazz)
   {
   const UDATA mask = (J9AccClassReferenceWeak |
                       J9AccClassReferenceSoft |
                       J9AccClassFinalizeNeeded |
                       J9AccClassOwnableSynchronizer);

#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      uintptr_t classDepthAndFlags = 0;
      JITServerHelpers::getAndCacheRAMClassInfo((J9Class *)clazz, TR::compInfoPT->getClientData(), stream, JITServerHelpers::CLASSINFO_CLASS_DEPTH_AND_FLAGS, (void *)&classDepthAndFlags);
      return ((classDepthAndFlags & mask)?true:false);
      }
   else
#endif /* defined(J9VM_OPT_JITSERVER) */
      {
      if (((J9Class *)clazz)->classDepthAndFlags & mask)
         {
         return true;
         }
      }

   return false;
   }

uintptr_t
J9::ClassEnv::classFlagsValue(TR_OpaqueClassBlock * classPointer)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      stream->write(JITServer::MessageType::ClassEnv_classFlagsValue, classPointer);
      return std::get<0>(stream->read<uintptr_t>());
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return (TR::Compiler->cls.convertClassOffsetToClassPtr(classPointer)->classFlags);
   }

uintptr_t
J9::ClassEnv::classFlagReservableWordInitValue(TR_OpaqueClassBlock * classPointer)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      uintptr_t classFlags = 0;
      JITServerHelpers::getAndCacheRAMClassInfo((J9Class *)classPointer, TR::compInfoPT->getClientData(), stream, JITServerHelpers::CLASSINFO_CLASS_FLAGS, (void *)&classFlags);
#ifdef DEBUG
      stream->write(JITServer::MessageType::ClassEnv_classFlagsValue, classPointer);
      uintptr_t classFlagsRemote = std::get<0>(stream->read<uintptr_t>());
      // Check that class flags from remote call is equal to the cached ones
      classFlags = classFlags & J9ClassReservableLockWordInit;
      classFlagsRemote = classFlagsRemote & J9ClassReservableLockWordInit;
      TR_ASSERT(classFlags == classFlagsRemote, "remote call class flags is not equal to cached class flags");
#endif
      return classFlags & J9ClassReservableLockWordInit;
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return (TR::Compiler->cls.convertClassOffsetToClassPtr(classPointer)->classFlags) & J9ClassReservableLockWordInit;
   }

uintptr_t
J9::ClassEnv::classDepthOf(TR_OpaqueClassBlock * clazzPointer)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      uintptr_t classDepthAndFlags = 0;
      JITServerHelpers::getAndCacheRAMClassInfo((J9Class *)clazzPointer, TR::compInfoPT->getClientData(), stream, JITServerHelpers::CLASSINFO_CLASS_DEPTH_AND_FLAGS, (void *)&classDepthAndFlags);
      return (classDepthAndFlags & J9AccClassDepthMask);
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return J9CLASS_DEPTH(TR::Compiler->cls.convertClassOffsetToClassPtr(clazzPointer));
   }


uintptr_t
J9::ClassEnv::classInstanceSize(TR_OpaqueClassBlock * clazzPointer)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      uintptr_t totalInstanceSize = 0;
      JITServerHelpers::getAndCacheRAMClassInfo((J9Class *)clazzPointer, TR::compInfoPT->getClientData(), stream, JITServerHelpers::CLASSINFO_TOTAL_INSTANCE_SIZE, (void *)&totalInstanceSize);
      return totalInstanceSize;
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return TR::Compiler->cls.convertClassOffsetToClassPtr(clazzPointer)->totalInstanceSize;
   }


J9ROMClass *
J9::ClassEnv::romClassOf(TR_OpaqueClassBlock * clazz)
   {
   J9Class *j9clazz = TR::Compiler->cls.convertClassOffsetToClassPtr(clazz);
#if defined(J9VM_OPT_JITSERVER)
   if (TR::compInfoPT && TR::compInfoPT->getStream())
      {
      return TR::compInfoPT->getAndCacheRemoteROMClass(j9clazz);
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return j9clazz->romClass;
   }

J9Class **
J9::ClassEnv::superClassesOf(TR_OpaqueClassBlock * clazz)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      stream->write(JITServer::MessageType::ClassEnv_superClassesOf, clazz);
      return std::get<0>(stream->read<J9Class **>());
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return TR::Compiler->cls.convertClassOffsetToClassPtr(clazz)->superclasses;
   }

J9ROMClass *
J9::ClassEnv::romClassOfSuperClass(TR_OpaqueClassBlock * clazz, size_t index)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      stream->write(JITServer::MessageType::ClassEnv_indexedSuperClassOf, clazz, index);
      J9Class *j9clazz = std::get<0>(stream->read<J9Class *>());
      return TR::compInfoPT->getAndCacheRemoteROMClass(j9clazz);
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return self()->superClassesOf(clazz)[index]->romClass;
   }

J9ITable *
J9::ClassEnv::iTableOf(TR_OpaqueClassBlock * clazz)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      stream->write(JITServer::MessageType::ClassEnv_iTableOf, clazz);
      return std::get<0>(stream->read<J9ITable*>());
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return (J9ITable*) self()->convertClassOffsetToClassPtr(clazz)->iTable;
   }

J9ITable *
J9::ClassEnv::iTableNext(J9ITable *current)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      stream->write(JITServer::MessageType::ClassEnv_iTableNext, current);
      return std::get<0>(stream->read<J9ITable*>());
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return current->next;
   }

J9ROMClass *
J9::ClassEnv::iTableRomClass(J9ITable *current)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      stream->write(JITServer::MessageType::ClassEnv_iTableRomClass, current);
      return std::get<0>(stream->read<J9ROMClass*>());
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return current->interfaceClass->romClass;
   }

#if defined(J9VM_OPT_JITSERVER)
std::vector<TR_OpaqueClassBlock *>
J9::ClassEnv::getITable(TR_OpaqueClassBlock *clazz)
   {
   if (auto stream = TR::CompilationInfo::getStream())
      {
      // This normally shouldn't be called from the server,
      // because it will have a cached table
      stream->write(JITServer::MessageType::ClassEnv_getITable, clazz);
      return std::get<0>(stream->read<std::vector<TR_OpaqueClassBlock *>>());
      }
   std::vector<TR_OpaqueClassBlock *> iTableSerialization;
   iTableSerialization.reserve((TR::Compiler->cls.romClassOf(clazz)->interfaceCount));
   for (J9ITable *iTableCur = TR::Compiler->cls.iTableOf(clazz); iTableCur; iTableCur = iTableCur->next)
      iTableSerialization.push_back((TR_OpaqueClassBlock *) iTableCur->interfaceClass);
   return iTableSerialization;
   }
#endif /* defined(J9VM_OPT_JITSERVER) */

bool
J9::ClassEnv::isStringClass(TR_OpaqueClassBlock *clazz)
   {
   //return (J9Class*)clazz == J9VMJAVALANGSTRING(jitConfig->javaVM);
   return false;
   }


bool
J9::ClassEnv::isStringClass(uintptr_t objectPointer)
   {
   /*
   TR_ASSERT(TR::Compiler->vm.hasAccess(omrVMThread), "isString requires VM access");
   return isString(getObjectClass(objectPointer));
   */
   return false;
   }

bool
J9::ClassEnv::isAbstractClass(TR::Compilation *comp, TR_OpaqueClassBlock *clazzPointer)
   {
   return comp->fej9()->isAbstractClass(clazzPointer);
   }

bool
J9::ClassEnv::isInterfaceClass(TR::Compilation *comp, TR_OpaqueClassBlock *clazzPointer)
   {
   return comp->fej9()->isInterfaceClass(clazzPointer);
   }

bool
J9::ClassEnv::isEnumClass(TR::Compilation *comp, TR_OpaqueClassBlock *clazzPointer, TR_ResolvedMethod *method)
   {
   return comp->fej9()->isEnumClass(clazzPointer, method);
   }

bool
J9::ClassEnv::isPrimitiveClass(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isPrimitiveClass(clazz);
   }

bool
J9::ClassEnv::isPrimitiveArray(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isPrimitiveArray(clazz);
   }

bool
J9::ClassEnv::isReferenceArray(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isReferenceArray(clazz);
   }

bool
J9::ClassEnv::isClassArray(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isClassArray(clazz);
   }

bool
J9::ClassEnv::isClassFinal(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isClassFinal(clazz);
   }

bool
J9::ClassEnv::hasFinalizer(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->hasFinalizer(clazz);
   }

bool
J9::ClassEnv::isClassInitialized(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isClassInitialized(clazz);
   }

bool
J9::ClassEnv::classHasIllegalStaticFinalFieldModification(TR_OpaqueClassBlock * clazzPointer)
   {
   J9Class* j9clazz = TR::Compiler->cls.convertClassOffsetToClassPtr(clazzPointer);
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      // J9ClassHasIllegalFinalFieldModifications bit is cached by ClientSessionData::processIllegalFinalFieldModificationList()
      // before the compilation takes place.
      uintptr_t classFlags = 0;
      JITServerHelpers::getAndCacheRAMClassInfo(j9clazz, TR::compInfoPT->getClientData(), stream, JITServerHelpers::CLASSINFO_CLASS_FLAGS, (void *)&classFlags);
      return J9_ARE_ANY_BITS_SET(classFlags, J9ClassHasIllegalFinalFieldModifications);
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   return J9_ARE_ANY_BITS_SET(j9clazz->classFlags, J9ClassHasIllegalFinalFieldModifications);
   }

bool
J9::ClassEnv::hasFinalFieldsInClass(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->hasFinalFieldsInClass(clazz);
   }

bool
J9::ClassEnv::sameClassLoaders(TR::Compilation *comp, TR_OpaqueClassBlock *class1, TR_OpaqueClassBlock *class2)
   {
   return comp->fej9()->sameClassLoaders(class1, class2);
   }

bool
J9::ClassEnv::isString(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isString(clazz);
   }

bool
J9::ClassEnv::isString(TR::Compilation *comp, uintptr_t objectPointer)
   {
   return comp->fej9()->isString(objectPointer);
   }

bool
J9::ClassEnv::jitStaticsAreSame(
      TR::Compilation *comp,
      TR_ResolvedMethod * method1,
      int32_t cpIndex1,
      TR_ResolvedMethod * method2,
      int32_t cpIndex2)
   {
   return comp->fej9()->jitStaticsAreSame(method1, cpIndex1, method2, cpIndex2);
   }

bool
J9::ClassEnv::jitFieldsAreSame(
      TR::Compilation *comp,
      TR_ResolvedMethod * method1,
      int32_t cpIndex1,
      TR_ResolvedMethod * method2,
      int32_t cpIndex2,
      int32_t isStatic)
   {
   return comp->fej9()->jitFieldsAreSame(method1, cpIndex1, method2, cpIndex2, isStatic);
   }

bool
J9::ClassEnv::isAnonymousClass(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->isAnonymousClass(clazz);
   }

extern J9Class *getFieldType_VMhelper(const char *fieldName, J9Class *definingClass)
   {
   UDATA numberOfFlattenedFields = definingClass->flattenedClassCache->numberOfEntries;
   for (UDATA i = 0; i < numberOfFlattenedFields; i++)
      {
      J9FlattenedClassCacheEntry *entry = J9_VM_FCC_ENTRY_FROM_CLASS(definingClass, i);
      J9ROMFieldShape *entryField = entry->field;
      J9UTF8 *name = J9ROMFIELDSHAPE_NAME(entryField);
      int length = J9UTF8_LENGTH(name);
      if (strlen(fieldName) == length &&
         !strncmp(fieldName, utf8Data(name), length))
         return entry->clazz;
      }
   TR_ASSERT_FATAL(false, "field %s doesn't exist in given class %p\n", fieldName, definingClass);
   }

static bool isFieldFlattened_VMHelper(const char *fieldName, J9Class *definingClass)
   {
   UDATA numberOfFlattenedFields = definingClass->flattenedClassCache->numberOfEntries;
   for (UDATA i = 0; i < numberOfFlattenedFields; i++)
      {
      J9FlattenedClassCacheEntry *entry = J9_VM_FCC_ENTRY_FROM_CLASS(definingClass, i);
      J9ROMFieldShape *entryField = entry->field;
      J9UTF8 *name = J9ROMFIELDSHAPE_NAME(entryField);
      int length = J9UTF8_LENGTH(name);
      if (strlen(fieldName) == length &&
         !strncmp(fieldName, utf8Data(name), length))
         {
         return J9_ARE_ANY_BITS_SET(entry->clazz->classFlags, J9ClassIsFlattened);
         }
      }
   return false;
   }

extern int getFieldOffset_VMHelper(const char *fieldName, J9Class *definingClass)
   {
   J9Class *j9class = reinterpret_cast<J9Class*>(definingClass);
   UDATA numberOfFlattenedFields = j9class->flattenedClassCache->numberOfEntries;
   for (UDATA i = 0; i < numberOfFlattenedFields; i++)
      {
      J9FlattenedClassCacheEntry *entry = J9_VM_FCC_ENTRY_FROM_CLASS(j9class, i);
      J9ROMFieldShape *entryField = entry->field;
      J9UTF8 *name = J9ROMFIELDSHAPE_NAME(entryField);
      int length = J9UTF8_LENGTH(name);
      if (strlen(fieldName) == length &&
         !strncmp(fieldName, utf8Data(name), length))
         return entry->offset;
      }
   TR_ASSERT_FATAL(false, "field %s doesn't exist in given class %p\n", fieldName, definingClass);
   }

bool
J9::ClassEnv::isFieldFlattened(TR::Compilation *comp, TR::SymbolReference * symRef)
   {
   TR_ResolvedJ9Method * j9method = static_cast<TR_ResolvedJ9Method *>(symRef->getOwningMethod(comp));
   bool isStatic;
   TR_OpaqueClassBlock * containingClass = j9method->definingClassFromCPFieldRef(comp, symRef->getCPIndex(), isStatic);
   int32_t nameLength ;
   const char * fieldname = j9method->fieldNameChars(symRef->getCPIndex(), nameLength);
   char fieldNameBuffer[50];
   strncpy(fieldNameBuffer, fieldname, nameLength);
   fieldNameBuffer[nameLength]='\0';
   return isFieldFlattened_VMHelper(fieldNameBuffer, reinterpret_cast<J9Class *>(containingClass));
   }

/*
 * \param prefix
 *    prefix is ended with `.`
 */
char * mergeFieldNames(char * prefix, char * fieldName, bool withDotAtTheEnd, TR::Region &region)
   {
   int prefixLength = prefix ? strlen(prefix) : 0;
   int nameLength = strlen(fieldName);
   int mergedLength = prefixLength;
   mergedLength+= nameLength;
   if (withDotAtTheEnd)
      mergedLength++;
   mergedLength++; /* for adding \0 at the end */;

   char * newName = new (region) char[mergedLength];
   if (prefixLength > 0)
      strncpy(newName, prefix, prefixLength);
   strncpy(newName + prefixLength, fieldName, nameLength);
   if (withDotAtTheEnd)
      newName[mergedLength-2] = '.';
   newName[mergedLength-1] = '\0';
   return newName;
   }

void addEntryForField(TR_VMField *field, TR::TypeLayoutBuilder &tlb, TR::Region& region, J9Class * definingClass, char * prefix, int offsetBase, TR::Compilation * comp)
   {
   if (isFieldFlattened_VMHelper(field->name, definingClass))
      {
      traceMsg(comp, "field %s is flattened\n", field->name);
      char * newPrefix = mergeFieldNames(prefix, field->name, true /*withDotAtTheEnd*/, comp->trMemory()->currentStackRegion());
      int newOffsetBase = field->offset + offsetBase;
      traceMsg(comp, "offset from TR_VMField %d, offset from fcc %d\n", field->offset, getFieldOffset_VMHelper(field->name, definingClass));
      J9Class *fieldClass = getFieldType_VMhelper(field->name, definingClass);
      TR_VMFieldsInfo fieldsInfo(comp, fieldClass, 1, stackAlloc);
      ListIterator<TR_VMField> iter(fieldsInfo.getFields());
      for (TR_VMField *childField = iter.getFirst(); childField; childField = iter.getNext())
         {
         addEntryForField(childField, tlb, region, fieldClass, newPrefix, newOffsetBase, comp);
         }
      }
   else
      {
      char *signature = field->signature;
      char charSignature = *signature;
      TR::DataType dataType;
      switch(charSignature)
         {
         case 'Z':
         case 'B':
         case 'C':
         case 'S':
         case 'I':
            {
            dataType = TR::Int32;
            break;
            }
         case 'J':
            {
            dataType = TR::Int64;
            break;
            }
         case 'F':
            {
            dataType = TR::Float;
            break;
            }
         case 'D':
            {
            dataType = TR::Double;
            break;
            }
// VALHALLA_TODO:  Might require different TR::DataType for value types (Q)
         case 'L':
         case 'Q':
         case '[':
            {
            dataType = TR::Address;
            break;
            }
         }


      char *fieldName = mergeFieldNames(prefix, field->name, false /* withDotAtTheEnd */ , region);
      int32_t offset = offsetBase + field->offset + TR::Compiler->om.objectHeaderSizeInBytes();
      bool isVolatile = (field->modifiers & J9AccVolatile) ? true : false;
      bool isPrivate = (field->modifiers & J9AccPrivate) ? true : false;
      bool isFinal = (field->modifiers & J9AccFinal) ? true : false;
      traceMsg(comp, "type layout definingClass %p field: %s, field offset: %d offsetBase %d\n", definingClass, fieldName, field->offset, offsetBase);
      tlb.add(TR::TypeLayoutEntry(dataType, offset, fieldName, isVolatile, isPrivate, isFinal, signature));
      }
   }

const TR::TypeLayout*
J9::ClassEnv::enumerateFields(TR::Region& region, TR_OpaqueClassBlock * opaqueClazz, TR::Compilation *comp)
   {
   TR_VMFieldsInfo fieldsInfo(comp, reinterpret_cast<J9Class*>(opaqueClazz), 1, stackAlloc);
   ListIterator<TR_VMField> iter(fieldsInfo.getFields());
   TR::TypeLayoutBuilder tlb(region);
   //findIndexInFlattenedClassCache
   for (TR_VMField *field = iter.getFirst(); field; field = iter.getNext())
      {
      addEntryForField(field, tlb, region, reinterpret_cast<J9Class* >(opaqueClazz), NULL, 0, comp);
      }
   return tlb.build();
   }

int32_t
J9::ClassEnv::vTableSlot(TR::Compilation *comp, TR_OpaqueMethodBlock *method, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->getVTableSlot(method, clazz);
   }


int32_t
J9::ClassEnv::flagValueForPrimitiveTypeCheck(TR::Compilation *comp)
   {
   return comp->fej9()->getFlagValueForPrimitiveTypeCheck();
   }


int32_t
J9::ClassEnv::flagValueForArrayCheck(TR::Compilation *comp)
   {
   return comp->fej9()->getFlagValueForArrayCheck();
   }


int32_t
J9::ClassEnv::flagValueForFinalizerCheck(TR::Compilation *comp)
   {
   return comp->fej9()->getFlagValueForFinalizerCheck();
   }


// this should be a method of TR_SymbolReference
char *
J9::ClassEnv::classNameChars(TR::Compilation *comp, TR::SymbolReference * symRef, int32_t & length)
   {
   return comp->fej9()->classNameChars(comp, symRef, length);
   }


char *
J9::ClassEnv::classNameChars(TR::Compilation *comp, TR_OpaqueClassBlock *clazz, int32_t & length)
   {
   return comp->fej9()->getClassNameChars(clazz, length);
   }


char *
J9::ClassEnv::classSignature_DEPRECATED(TR::Compilation *comp, TR_OpaqueClassBlock * clazz, int32_t & length, TR_Memory *m)
   {
   return comp->fej9()->getClassSignature_DEPRECATED(clazz, length, m);
   }


char *
J9::ClassEnv::classSignature(TR::Compilation *comp, TR_OpaqueClassBlock * clazz, TR_Memory *m)
   {
   return comp->fej9()->getClassSignature(clazz, m);
   }

uintptr_t
J9::ClassEnv::persistentClassPointerFromClassPointer(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
   return comp->fej9()->getPersistentClassPointerFromClassPointer(clazz);
   }

TR_OpaqueClassBlock *
J9::ClassEnv::objectClass(TR::Compilation *comp, uintptr_t objectPointer)
   {
   return comp->fej9()->getObjectClass(objectPointer);
   }

TR_OpaqueClassBlock *
J9::ClassEnv::classFromJavaLangClass(TR::Compilation *comp, uintptr_t objectPointer)
   {
   return comp->fej9()->getClassFromJavaLangClass(objectPointer);
   }


uint16_t
J9::ClassEnv::getStringCharacter(TR::Compilation *comp, uintptr_t objectPointer, int32_t index)
   {
   return comp->fej9()->getStringCharacter(objectPointer, index);
   }


bool
J9::ClassEnv::getStringFieldByName(TR::Compilation *comp, TR::SymbolReference *stringRef, TR::SymbolReference *fieldRef, void* &pResult)
   {
   return comp->fej9()->getStringFieldByName(comp, stringRef, fieldRef, pResult);
   }

uintptr_t
J9::ClassEnv::getArrayElementWidthInBytes(TR::Compilation *comp, TR_OpaqueClassBlock* arrayClass)
   {
   TR_ASSERT(TR::Compiler->cls.isClassArray(comp, arrayClass), "Class must be array");
   int32_t logElementSize = ((J9ROMArrayClass*)((J9Class*)arrayClass)->romClass)->arrayShape & 0x0000FFFF;
   return 1 << logElementSize;
   }

intptr_t
J9::ClassEnv::getVFTEntry(TR::Compilation *comp, TR_OpaqueClassBlock* clazz, int32_t offset)
   {
   return comp->fej9()->getVFTEntry(clazz, offset);
   }

uint8_t *
J9::ClassEnv::getROMClassRefName(TR::Compilation *comp, TR_OpaqueClassBlock *clazz, uint32_t cpIndex, int &classRefLen)
   {
   J9ROMConstantPoolItem *romCP = self()->getROMConstantPool(comp, clazz);
#if defined(J9VM_OPT_JITSERVER)
   if (comp->isOutOfProcessCompilation())
      {
      J9ROMFieldRef *romFieldRef = (J9ROMFieldRef *)&romCP[cpIndex];
      TR_ASSERT(JITServerHelpers::isAddressInROMClass(romFieldRef, self()->romClassOf(clazz)), "Field ref must be in ROM class");

      J9ROMClassRef *romClassRef = (J9ROMClassRef *)&romCP[romFieldRef->classRefCPIndex];
      TR_ASSERT(JITServerHelpers::isAddressInROMClass(romClassRef, self()->romClassOf(clazz)), "Class ref must be in ROM class");

      TR::CompilationInfoPerThread *compInfoPT = TR::compInfoPT;
      char *name = NULL;

      OMR::CriticalSection getRemoteROMClass(compInfoPT->getClientData()->getROMMapMonitor());
      auto &classMap = compInfoPT->getClientData()->getROMClassMap();
      auto it = classMap.find(reinterpret_cast<J9Class *>(clazz));
      auto &classInfo = it->second;
      name = classInfo.getROMString(classRefLen, romClassRef,
                             {
                             offsetof(J9ROMClassRef, name)
                             });
      return (uint8_t *) name;
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   J9ROMFieldRef *romFieldRef = (J9ROMFieldRef *)&romCP[cpIndex];
   J9ROMClassRef *romClassRef = (J9ROMClassRef *)&romCP[romFieldRef->classRefCPIndex];
   J9UTF8 *classRefNameUtf8 = J9ROMCLASSREF_NAME(romClassRef);
   classRefLen = J9UTF8_LENGTH(classRefNameUtf8);
   uint8_t *classRefName = J9UTF8_DATA(classRefNameUtf8);
   return classRefName;
   }

J9ROMConstantPoolItem *
J9::ClassEnv::getROMConstantPool(TR::Compilation *comp, TR_OpaqueClassBlock *clazz)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (comp->isOutOfProcessCompilation())
      {
      J9ROMClass *romClass = TR::compInfoPT->getAndCacheRemoteROMClass(reinterpret_cast<J9Class *>(clazz));
      return (J9ROMConstantPoolItem *) ((UDATA) romClass + sizeof(J9ROMClass));
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   J9ConstantPool *ramCP = reinterpret_cast<J9ConstantPool *>(comp->fej9()->getConstantPoolFromClass(clazz));
   return ramCP->romConstantPool;
   }

bool
J9::ClassEnv::isValueTypeClass(TR_OpaqueClassBlock *clazz)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      uintptr_t classFlags = 0;
      JITServerHelpers::getAndCacheRAMClassInfo((J9Class *)clazz, TR::compInfoPT->getClientData(), stream, JITServerHelpers::CLASSINFO_CLASS_FLAGS, (void *)&classFlags);
#ifdef DEBUG
      stream->write(JITServer::MessageType::ClassEnv_classFlagsValue, clazz);
      uintptr_t classFlagsRemote = std::get<0>(stream->read<uintptr_t>());
      // Check that class flags from remote call is equal to the cached ones
      classFlags = classFlags & J9ClassIsValueType;
      classFlagsRemote = classFlagsRemote & J9ClassIsValueType;
      TR_ASSERT(classFlags == classFlagsRemote, "remote call class flags is not equal to cached class flags");
#endif
      return classFlags & J9ClassIsValueType;
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   J9Class *j9class = reinterpret_cast<J9Class*>(clazz);
   return J9_IS_J9CLASS_VALUETYPE(j9class);
   }

bool
J9::ClassEnv::isZeroInitializable(TR_OpaqueClassBlock *clazz)
   {
#if defined(J9VM_OPT_JITSERVER)
   if (auto stream = TR::CompilationInfo::getStream())
      {
      uintptr_t classFlags = 0;
      JITServerHelpers::getAndCacheRAMClassInfo((J9Class *)clazz, TR::compInfoPT->getClientData(), stream, JITServerHelpers::CLASSINFO_CLASS_FLAGS, (void *)&classFlags);
#ifdef DEBUG
      stream->write(JITServer::MessageType::ClassEnv_classFlagsValue, clazz);
      uintptr_t classFlagsRemote = std::get<0>(stream->read<uintptr_t>());
      // Check that class flags from remote call is equal to the cached ones
      classFlags = classFlags & J9ClassContainsUnflattenedFlattenables;
      classFlagsRemote = classFlagsRemote & J9ClassContainsUnflattenedFlattenables;
      TR_ASSERT(classFlags == classFlagsRemote, "remote call class flags is not equal to cached class flags");
#endif
      return classFlags & J9ClassContainsUnflattenedFlattenables;
      }
#endif
   return (self()->classFlagsValue(clazz) & J9ClassContainsUnflattenedFlattenables) == 0;
   }

bool
J9::ClassEnv::containsZeroOrOneConcreteClass(TR::Compilation *comp, List<TR_PersistentClassInfo>* subClasses)
   {
   int count = 0;
#if defined(J9VM_OPT_JITSERVER)
   if (comp->isOutOfProcessCompilation())
      {
      ListIterator<TR_PersistentClassInfo> j(subClasses);
      TR_ScratchList<TR_PersistentClassInfo> subClassesNotCached(comp->trMemory());

      // Process classes cached at the server first
      ClientSessionData * clientData = TR::compInfoPT->getClientData();
      for (TR_PersistentClassInfo *ptClassInfo = j.getFirst(); ptClassInfo; ptClassInfo = j.getNext())
         {
         TR_OpaqueClassBlock *clazz = ptClassInfo->getClassId();
         J9Class *j9clazz = TR::Compiler->cls.convertClassOffsetToClassPtr(clazz);
         auto romClass = JITServerHelpers::getRemoteROMClassIfCached(clientData, j9clazz);
         if (romClass == NULL)
            {
            subClassesNotCached.add(ptClassInfo);
            }
         else
            {
            if (!TR::Compiler->cls.isInterfaceClass(comp, clazz) && !TR::Compiler->cls.isAbstractClass(comp, clazz))
               {
               if (++count > 1)
                  return false;
               }
            }
         }
      // Traverse through classes that are not cached on server
      ListIterator<TR_PersistentClassInfo> i(&subClassesNotCached);
      for (TR_PersistentClassInfo *ptClassInfo = i.getFirst(); ptClassInfo; ptClassInfo = i.getNext())
         {
         TR_OpaqueClassBlock *clazz = ptClassInfo->getClassId();
         if (!TR::Compiler->cls.isInterfaceClass(comp, clazz) && !TR::Compiler->cls.isAbstractClass(comp, clazz))
            {
            if (++count > 1)
               return false;
            }
         }
      }
   else // non-jitserver
#endif /* defined(J9VM_OPT_JITSERVER) */
      {
      ListIterator<TR_PersistentClassInfo> i(subClasses);
      for (TR_PersistentClassInfo *ptClassInfo = i.getFirst(); ptClassInfo; ptClassInfo = i.getNext())
         {
         TR_OpaqueClassBlock *clazz = ptClassInfo->getClassId();
         if (!TR::Compiler->cls.isInterfaceClass(comp, clazz) && !TR::Compiler->cls.isAbstractClass(comp, clazz))
            {
            if (++count > 1)
               return false;
            }
         }
      }
   return true;
   }

bool
J9::ClassEnv::isClassRefValueType(TR::Compilation *comp, TR_OpaqueClassBlock *cpContextClass, int32_t cpIndex)
   {
   J9Class * j9class = reinterpret_cast<J9Class *>(cpContextClass);
   J9JavaVM *vm = getJ9JitConfigFromFE(comp->fej9())->javaVM;
   return vm->internalVMFunctions->isClassRefQtype(j9class, cpIndex);
   }
