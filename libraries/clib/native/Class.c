/*
 * java.lang.Class.c
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#include "config.h"
#include "config-std.h"
#include "config-mem.h"
#include "../../../kaffe/kaffevm/gtypes.h"
#include "../../../kaffe/kaffevm/access.h"
#include "../../../kaffe/kaffevm/constants.h"
#include "../../../kaffe/kaffevm/object.h"
#include "../../../kaffe/kaffevm/classMethod.h"
#include "../../../kaffe/kaffevm/itypes.h"
#include "../../../kaffe/kaffevm/support.h"
#include "../../../kaffe/kaffevm/stringSupport.h"
#include "../../../kaffe/kaffevm/soft.h"
#include "../../../kaffe/kaffevm/stackTrace.h"
#include "../../../kaffe/kaffevm/baseClasses.h"
#include "../../../kaffe/kaffevm/lookup.h"
#include "java_io_InputStream.h"
#include "java_io_PrintStream.h"
#include "java_lang_System.h"
#include "java_lang_reflect_Constructor.h"
#include "java_lang_reflect_Method.h"
#include "java_lang_reflect_Field.h"
#include <native.h>
#include "defs.h"

extern Hjava_lang_Object* buildStackTrace(struct _exceptionFrame*);

/*
 * Convert string name to class object.
 */
struct Hjava_lang_Class*
java_lang_Class_forName(struct Hjava_lang_String* str, jbool doinit,
	Hjava_lang_ClassLoader* loader)
{
	errorInfo einfo;
	Hjava_lang_Class* clazz;
	Utf8Const *utf8buf;
	char buf[MAXNAMELEN];

	/* Get string and convert '.' to '/' */
	stringJava2CBuf(str, buf, sizeof(buf));
	classname2pathname(buf, buf);

	/*
	 * Note the following oddity: 
	 * 
	 * It is apparently perfectly legal to call forName for array types,
	 * such as "[Ljava.lang.String;" or "[B".  
	 * However, it is wrong to call Class.forName("Ljava.lang.String;")
	 *
	 * This situation is similar to the constant pool resolution.  We
	 * therefore do the same thing as in getClass in kaffevm/lookup.c,
	 * that is, use either loadArray or loadClass depending on the name.
	 *
	 * This is somewhat described in Section 5.1.3 of the VM 
	 * Specification, titled "Array Classes".  This section seems to 
	 * imply that we must avoid asking a class loader to resolve such
	 * array names (those starting with an [), and this is what calling
	 * loadArray does.
	 */
	utf8buf = checkPtr(utf8ConstNew(buf, -1));
	if (buf[0] == '[') {
		clazz = loadArray(utf8buf, loader, &einfo);
	}
	else {
		clazz = loadClass(utf8buf, loader, &einfo);
	}

	/* if an error occurred, throw an exception */
	if (clazz == 0) {
		/* The only checked exception that Class.forName() throws
		 * is ClassNotFoundException.  This is an exception, not an
		 * Error, which users often catch.
		 *
		 * However, Class.forName() can also throw errors, such as
		 * NoClassDefFoundError, if for instance a superclass for
		 * a class couldn't be found.
		 *
		 * When it throws which, we don't really know.  We try to be 
		 * compatible, so we upgrade the error to an exception if it's 
		 * (NoClassDefFoundError, this_class_name), or if it's a
		 * VerifyError.
		 * NB: 1.2 seems to be more consistent and throws 
		 * ClassNotFoundException in most cases.
		 */
		if ((einfo.type & KERR_EXCEPTION) && !strcmp(einfo.classname, "java.lang.VerifyError")) 
		{
			errorInfo einfo_copy = einfo;
			postExceptionMessage(&einfo, 
				JAVA_LANG(ClassNotFoundException), 
				einfo.mess);
			discardErrorInfo(&einfo_copy);
		} else
		if ((einfo.type & KERR_EXCEPTION) && !strcmp(einfo.classname, "java.lang.NoClassDefFoundError"))
		{
			/*
			 * However, we don't upgrade if it is a second attempt
			 * to load a class whose loading has already failed.
			 */
			classEntry* centry;
			centry = lookupClassEntry(utf8buf, loader, &einfo);
			if (centry == 0 || (centry->class &&
			    centry->class->state == CSTATE_FAILED)) {
				utf8ConstRelease(utf8buf);
				throwError(&einfo);
			}

			/* This is not quite what Sun does: they use the
			 * classname, we use the pathname as the message
			 * of the exception  (FIXME?)
			 */
			if (buf[0] == '[' || !strcmp(einfo.mess, buf)) {
				errorInfo einfo_copy = einfo;
				postExceptionMessage(&einfo,
					JAVA_LANG(ClassNotFoundException), 
					einfo.mess);
				discardErrorInfo(&einfo_copy);
			}
		}
		utf8ConstRelease(utf8buf);
		throwError(&einfo);
	}
	utf8ConstRelease(utf8buf);

	/*
	 * loadClass returns the class in state CSTATE_LINKED.
	 *
	 * Processing to CSTATE_COMPLETE will initialize the class, resolve
	 * its constants and run its static initializers.
	 *
	 * The option to load a class via forName without initializing it
	 * was introduced in 1.2, presumably for the convenience of
	 * programs such as stub compilers.
	 */
	if (doinit && processClass(clazz, CSTATE_COMPLETE, &einfo) == false) {
		throwError(&einfo);
	}
	return (clazz);
}

struct Hjava_lang_Class*
java_lang_Class_forName0(struct Hjava_lang_String* str)
{
        int i;
	Hjava_lang_ClassLoader* loader;
	stackTraceInfo* info;
	static Method * cfnmeth;

	if (!cfnmeth) {
		Utf8Const *fname = checkPtr(utf8ConstNew("forName", -1));
		Utf8Const *fsig = utf8ConstNew("(Ljava/lang/String;)Ljava/lang/Class;", -1);
		if (!fsig) {
			errorInfo einfo;

			utf8ConstRelease(fname);
			postOutOfMemory(&einfo);
			throwError(&einfo);
		}
		cfnmeth = findMethodLocal(ClassClass, fname, fsig);
		utf8ConstRelease(fname);
		utf8ConstRelease(fsig);
	}

	if (cfnmeth == 0) {
		abort();
	}

        /*
         * If the calling method is in a class that was loaded by a class
         * loader, use that class loader to find the class corresponding to
         * the name.  Otherwise, use the system class loader.
         */
        loader = 0;
        info = (stackTraceInfo*)buildStackTrace(0);
	if (!info) {
		struct _errorInfo info;
		postOutOfMemory(&info);
		throwError(&info);
	}
        for (i = 0; info[i].meth != ENDOFSTACK; i++) {
                info[i].meth = stacktraceFindMethod(&info[i]);
		/* skip java.lang.Class.forName(String) cause that's
		 * who called us
		 */
		if (info[i].meth == cfnmeth) {
			continue;
		}

                if (info[i].meth != 0 && info[i].meth->class != 0) {
                        loader = info[i].meth->class->loader;
                        break;
                }
        }
	return (java_lang_Class_forName(str, true, loader));
}

/*
 * Convert class to string name.
 */
struct Hjava_lang_String*
java_lang_Class_getName(struct Hjava_lang_Class* c)
{
	return(checkPtr(utf8Const2JavaReplace(c->name, '/', '.')));
}

/*
 * Create a new instance of the derived class.
 */
struct Hjava_lang_Object*
java_lang_Class_newInstance(struct Hjava_lang_Class* this)
{
	if (CLASS_IS_PRIMITIVE(this)) {
		SignalError("java.lang.InstantiationException", 
			    CLASS_CNAME(this));
	}
	return (execute_java_constructor(0, this, "()V"));
}

/*
 * Return super class.
 *
 * Note that the specs demands to return null if the class object is an
 * interface or the class object representing java.lang.Object.
 *
 * That is, we're asked to NOT report superinterfaces for interfaces.
 * That would be impossible anyway since the spec says that the super_class
 * attribute in a class file describing an interface must point to
 * java.lang.Object.  An interface is considered to "implement" its
 * superinterface(s).  See also getInterfaceMethods0.
 */
struct Hjava_lang_Class*
java_lang_Class_getSuperclass(struct Hjava_lang_Class* this)
{
	if (!CLASS_IS_INTERFACE(this))
		return (this->superclass);
	else
		return (NULL);
}

HArrayOfObject* /* [Ljava.lang.Class; */
java_lang_Class_getInterfaces(struct Hjava_lang_Class* this)
{
	HArrayOfObject* obj;
	struct Hjava_lang_Class** ifaces;
	int i;
	int nr;

	nr = this->interface_len;

#if defined(JDK_1_1_COMPAT)
	/*
	 * Do not report java.io.Serializable for array classes in JDK 1.1
	 */
	if (CLASS_IS_ARRAY(this)) {
		nr = 0;
	}
#endif

	obj = (HArrayOfObject*)AllocObjectArray(nr, "Ljava/lang/Class");
	ifaces = (struct Hjava_lang_Class**)unhand_array(obj)->body;
	for (i = 0; i < nr; i++) {
		ifaces[i] = this->interfaces[i];
	}

	return (obj);
}

/*
 * Return the class loader which loaded me.
 */
struct Hjava_lang_ClassLoader*
java_lang_Class_getClassLoader0(struct Hjava_lang_Class* this)
{
	return (this->loader);
}

/*
 * Is the class an interface?
 */
jbool
java_lang_Class_isInterface(struct Hjava_lang_Class* this)
{
	return ((this->accflags & ACC_INTERFACE) ? 1 : 0);
}

jbool
java_lang_Class_isPrimitive(struct Hjava_lang_Class* this)
{
	return (CLASS_IS_PRIMITIVE(this));
}

jbool
java_lang_Class_isArray(struct Hjava_lang_Class* this)
{
	return (CLASS_IS_ARRAY(this));
}

Hjava_lang_Class*
java_lang_Class_getComponentType(struct Hjava_lang_Class* this)
{
	if (CLASS_IS_ARRAY(this)) {
		return (CLASS_ELEMENT_TYPE(this));
	}
	else {
		return ((Hjava_lang_Class*)0);
	}
}

jbool
java_lang_Class_isAssignableFrom(struct Hjava_lang_Class* this, struct Hjava_lang_Class* cls)
{
	return (instanceof(this, cls));
}

/*
 * Get primitive class from class name (JDK 1.1)
 */
struct Hjava_lang_Class*
java_lang_Class_getPrimitiveClass(struct Hjava_lang_String* name)
{
	jchar* chrs;

	chrs = STRING_DATA(name);
	switch (chrs[0]) {
	case 'b':
		if (chrs[1] == 'y') {
			return (byteClass);
		}
		if (chrs[1] == 'o') {
			return (booleanClass);
		}
		break;
	case 'c':
		return (charClass);
	case 'd':
		return (doubleClass);
	case 'f':
		return (floatClass);
	case 'i':
		return (intClass);
	case 'l':
		return (longClass);
	case 's':
		return (shortClass);
	case 'v':
		return (voidClass);
	}
	return(NULL);
}

/*
 * Is object instance of this class?
 */
jbool
java_lang_Class_isInstance(struct Hjava_lang_Class* this, struct Hjava_lang_Object* obj)
{
	return (soft_instanceof(this, obj));
}

jint
java_lang_Class_getModifiers(struct Hjava_lang_Class* this)
{
	return (this->accflags & ACC_MASK);
}

HArrayOfObject*
java_lang_Class_getSigners(struct Hjava_lang_Class* this)
{
	unimp("java.lang.Class:getSigners unimplemented");
}

void
java_lang_Class_setSigners(struct Hjava_lang_Class* this, HArrayOfObject* sigs)
{
	unimp("java.lang.Class:setSigners unimplemented");
}

static
HArrayOfObject*
makeParameters(Method* meth)
{
	int i;
	HArrayOfObject* array;
	errorInfo info;
	Hjava_lang_Class* clazz;

	array = (HArrayOfObject*)AllocObjectArray(METHOD_NARGS(meth), "Ljava/lang/Class;");
	for (i = 0; i < METHOD_NARGS(meth); ++i) {
		clazz = getClassFromSignature(METHOD_ARG_TYPE(meth, i),
					      meth->class->loader, &info);
		if (clazz == 0) {
			throwError(&info);
		}
		unhand_array(array)->body[i] = &clazz->head;
	}

        return (array);
}

static
Hjava_lang_Class*
makeReturn(Method* meth)
{
	errorInfo info;
	Hjava_lang_Class* clazz;

	clazz = getClassFromSignature(METHOD_RET_TYPE(meth), meth->class->loader, &info);
	if (clazz == 0) {
		throwError(&info);
	}
	return (clazz);
}

/*
 * create an array of types for the checked exceptions that this method 
 * declared to throw.  These are stored in the declared_exception table
 * as indices into the constant pool.
 *
 * We do not bother to cache the resolved types here.
 */
static
HArrayOfObject*
makeExceptions(Method* meth)
{
	int nr;
	int i;
	HArrayOfObject* array;
	Hjava_lang_Class** ptr;

	nr = meth->ndeclared_exceptions;
	array = (HArrayOfObject*)AllocObjectArray(nr, "Ljava/lang/Class;");
	ptr = (Hjava_lang_Class**)&unhand_array(array)->body[0];
	for (i = 0; i < nr; i++) {
		errorInfo info;
		Hjava_lang_Class* clazz;
		clazz = getClass(meth->declared_exceptions[i], meth->class, 
				&info);
		if (clazz == 0) {
			throwError(&info);
		}
		*ptr++ = clazz;
	}
	return (array);
}

static
Hjava_lang_reflect_Constructor*
makeConstructor(struct Hjava_lang_Class* clazz, int slot)
{
	Hjava_lang_reflect_Constructor* meth;
	Method* mth;

	mth = CLASS_METHODS(clazz) + slot;
	meth = (Hjava_lang_reflect_Constructor*)AllocObject("java/lang/reflect/Constructor");      

	unhand(meth)->clazz = clazz;
	unhand(meth)->slot = slot;
	unhand(meth)->parameterTypes = makeParameters(mth);
	unhand(meth)->exceptionTypes = makeExceptions(mth);

	return (meth);
}

static
Hjava_lang_reflect_Method*
makeMethod(struct Hjava_lang_Class* clazz, int slot)
{
	Hjava_lang_reflect_Method* meth;
	Method* mth;

	mth = CLASS_METHODS(clazz) + slot;
	meth = (Hjava_lang_reflect_Method*)AllocObject("java/lang/reflect/Method");      

	unhand(meth)->clazz = clazz;
	unhand(meth)->slot = slot;
	unhand(meth)->name = checkPtr(utf8Const2Java(mth->name));
	unhand(meth)->parameterTypes = makeParameters(mth);
	unhand(meth)->exceptionTypes = makeExceptions(mth);
	unhand(meth)->returnType = makeReturn(mth);

	return (meth);
}

static
Hjava_lang_reflect_Field*
makeField(struct Hjava_lang_Class* clazz, int slot)
{
	Hjava_lang_reflect_Field* field;
	Field* fld;
	errorInfo info;

	fld = CLASS_FIELDS(clazz) + slot;
	field = (Hjava_lang_reflect_Field*)AllocObject("java/lang/reflect/Field");
	unhand(field)->clazz = clazz;
	unhand(field)->slot = slot;
	unhand(field)->type = resolveFieldType(fld, clazz, &info);
	if (unhand(field)->type == 0) {
		throwError(&info);
	}
	unhand(field)->name = checkPtr(utf8Const2Java(fld->name));
	return (field);
}
 
/*
 * count the number of methods in a class that are not constructors.
 * If declared is not set, count only public methods.
 */
static int
countMethods(struct Hjava_lang_Class* clas, jint declared)
{
	Method* mth = CLASS_METHODS(clas);
	int i;
	int count = 0;

	for (i = CLASS_NMETHODS(clas)-1 ; i >= 0; i--) {
		if (((mth[i].accflags & ACC_PUBLIC) || declared) && !(mth[i].accflags & ACC_CONSTRUCTOR)) {
			count++;
		}
	}
	return count;
}

/*
 * create reflect.Method objects for all methods in a class that are 
 * not constructors.  If declared is not set, include only public methods.
 */
static void
addMethods(struct Hjava_lang_Class* clas, jint declared, 
	Hjava_lang_reflect_Method*** ptr)
{
	Method* mth = CLASS_METHODS(clas);
	int i;

	for (i = CLASS_NMETHODS(clas)-1; i >= 0; i--) {
		if (((mth[i].accflags & ACC_PUBLIC) || declared) && !(mth[i].accflags & ACC_CONSTRUCTOR)) {
			**ptr = makeMethod(clas, i);
			(*ptr)++;
		}
	}
}

/*
 * Reflect all methods implemented by an interface or one of its 
 * superinterfaces.
 *
 * Note that we do not reach the "superinterface" via the superclass pointer.
 * See the VM Spec, which says:
 *
 *   "The implements clause in a [interface] class declaration lists the 
 *    names of interfaces that are direct superinterfaces of the [interface]
 *    class being declared."
 *
 * Hence we must look at the interfaces list for superinterfaces.
 */
static
HArrayOfObject*
getInterfaceMethods0(struct Hjava_lang_Class* this, jint declared)
{
	int count;
	Hjava_lang_reflect_Method** ptr;
	HArrayOfObject* array;
	int i;

	count = 0;
	count += countMethods(this, declared);
	if (!declared) {
		for (i = 0; i < this->total_interface_len; i++) {
			count += countMethods(this->interfaces[i], declared);
		}
	}

	array = (HArrayOfObject*)AllocObjectArray(count, "Ljava/lang/reflect/Method;");
	ptr = (Hjava_lang_reflect_Method**)&unhand_array(array)->body[0];

	addMethods(this, declared, &ptr);
	if (!declared) {
		for (i = 0; i < this->total_interface_len; i++) {
			addMethods(this->interfaces[i], declared, &ptr);
		}
	}

	return (array);
}

HArrayOfObject*
java_lang_Class_getMethods0(struct Hjava_lang_Class* this, jint declared)
{
	int count;
	Hjava_lang_Class* clas;
	Hjava_lang_reflect_Method** ptr;
	HArrayOfObject* array;

	/*
	 * Note: the spec wants us to include the methods of all superclasses
	 * and all superinterfaces.  
	 *
	 * Superinterfaces cannot be reached through the superclass
	 * pointer.  We handle them in a separate function.
	 */
	if (CLASS_IS_INTERFACE(this))
		return (getInterfaceMethods0(this, declared));

	count = 0;
	for (clas = this; clas != NULL; clas = clas->superclass) {

		count += countMethods(clas, declared);

		if (declared) {
			break;
		}
	}
	array = (HArrayOfObject*)AllocObjectArray(count, "Ljava/lang/reflect/Method;");
	ptr = (Hjava_lang_reflect_Method**)&unhand_array(array)->body[0];
	for (clas = this; clas != NULL; clas = clas->superclass) {

		addMethods(clas, declared, &ptr);

		if (declared) {
			break;
		}
	}

	return (array);
}

HArrayOfObject*
java_lang_Class_getConstructors0(struct Hjava_lang_Class* this, jint declared)
{
	int count;
	Hjava_lang_Class* clas;
	Method* mth;
	Hjava_lang_reflect_Constructor** ptr;
	HArrayOfObject* array;
	int i;

	count = 0;
	clas = this;
	mth = CLASS_METHODS(clas);
	for (i = CLASS_NMETHODS(clas)-1; i >= 0;  i--) {
		if (((mth[i].accflags & ACC_PUBLIC) || declared) && (mth[i].accflags & ACC_CONSTRUCTOR)) {
			count++;
		}
	}
	array = (HArrayOfObject*)AllocObjectArray(count, "Ljava/lang/reflect/Constructor;");
	ptr = (Hjava_lang_reflect_Constructor**)&unhand_array(array)->body[0];
	clas = this;
	mth = CLASS_METHODS(clas);
	for (i = CLASS_NMETHODS(clas)-1; i >= 0;  i--) {
		if (((mth[i].accflags & ACC_PUBLIC) || declared) && (mth[i].accflags & ACC_CONSTRUCTOR)) {
			*ptr = makeConstructor(clas, i);
			ptr++;
		}
	}
	return (array);
}

HArrayOfObject*
java_lang_Class_getFields0(struct Hjava_lang_Class* clazz, int declared)
{
	int count;
	Hjava_lang_Class* clas;
	Field* fld;
	Hjava_lang_reflect_Field** ptr;
	HArrayOfObject* array;
	int i;

	if (declared) {
		count = CLASS_NFIELDS((Hjava_lang_Class*)clazz);
	}
	else {
		count = 0;
		for (clas = clazz; clas != NULL; clas = clas->superclass) {
			fld = CLASS_FIELDS(clas);
			for (i = CLASS_NFIELDS(clas)-1; i >= 0; i--) {
				if (fld[i].accflags & ACC_PUBLIC) {
					count++;
				}
			}
		}
	}
	array = (HArrayOfObject*)AllocObjectArray(count, "Ljava/lang/reflect/Field;");
	ptr = (Hjava_lang_reflect_Field**)&unhand_array(array)->body[0];
	for (clas = clazz; clas != NULL; clas = clas->superclass) {
		fld = CLASS_FIELDS(clas);
		for (i = CLASS_NFIELDS(clas)-1; i >= 0;  i--) {
			if ((fld[i].accflags & ACC_PUBLIC) || declared) {
				*ptr = makeField(clas, i);
				ptr++;
			}
		}
		if (declared) {
			break;
		}
	}

	return (array);
}

/*
 * Check whether the parameters of Method `mth' have exactly the same
 * types as `argtypes', where argtypes is an array of Hjava_lang_Class *
 *
 * Note that checking the arguments might cause the resolution of names
 * that are part of the signature.  These must be resolved by the same 
 * classloader that loaded the class to which the method belongs.
 *
 * This function is used by getMethod0 and getConstructor0.
 *
 * Returns 1 if they are exactly the same, 0 if not.
 */
static
int
checkParameters(Method* mth, HArrayOfObject* argtypes)
{
	int i;
	errorInfo info;

	/* The JDK doc says and experimentation shows that a null second 
	 * parameter to all get(Declared){Method|Constructor} functions
	 * is treated like passing an empty array "new Class [] {}"
	 */
	if ((argtypes == NULL) && METHOD_NARGS(mth) == 0) {
		return (1);
	}

	if (ARRAY_SIZE(argtypes) != METHOD_NARGS(mth)) {
		return (0);
	}

	for (i = 0; i < ARRAY_SIZE(argtypes); i++) {
		Hjava_lang_Class* sigclass;
		sigclass = getClassFromSignature(METHOD_ARG_TYPE(mth, i), mth->class->loader, &info);
		if (sigclass == 0) {
			discardErrorInfo(&info);
			return 0;
		}
		if (sigclass !=
			(struct Hjava_lang_Class *)OBJARRAY_DATA(argtypes)[i]) {
			return (0);
		}
	}

	return (1);
}

Hjava_lang_reflect_Method*
java_lang_Class_getMethod0(struct Hjava_lang_Class* this, struct Hjava_lang_String* name, HArrayOfObject* arr, jint declared)
{
	Hjava_lang_Class* clas;

	clas = this;
	do {
		Method* mth = CLASS_METHODS(clas);
		int n = CLASS_NMETHODS(clas);
		int i;
		for (i = 0;  i < n;  ++mth, ++i) {
			if (((mth->accflags & ACC_PUBLIC) || declared)
			    && utf8ConstEqualJavaString(mth->name, name)) {
				if (checkParameters(mth, arr))
					return (makeMethod(clas, i));
			}
		}
		clas = clas->superclass;
	} while (!declared && clas != NULL);

	/* like SignalError, except that the name of the class that is
	 * not found becomes the error message 
	 */
	throwException((struct Hjava_lang_Throwable*)execute_java_constructor(
		"java.lang.NoSuchMethodException", 0, 
		"(Ljava/lang/String;)V", name));
}

struct Hjava_lang_reflect_Constructor*
java_lang_Class_getConstructor0(struct Hjava_lang_Class* this, HArrayOfObject* arr, jint declared) 
{
	Hjava_lang_Class* clas = this;

	Method* mth = CLASS_METHODS(clas);
	int n = CLASS_NMETHODS(clas);
	int i;
	for (i = 0;  i < n;  ++mth, ++i) {
		if (((mth->accflags & ACC_PUBLIC) || declared) && (mth->accflags & ACC_CONSTRUCTOR)) {
			if (checkParameters(mth, arr))
				return (makeConstructor(clas, i));
		}
	}

	SignalError("java.lang.NoSuchMethodException", ""); /* FIXME */
}

Hjava_lang_reflect_Field*
java_lang_Class_getField0(struct Hjava_lang_Class* clazz, struct Hjava_lang_String* name, int declared)
{
	Hjava_lang_Class* clas;

	clas = (Hjava_lang_Class*) clazz;
	do {
		Field* fld = CLASS_FIELDS(clas);
		int n = CLASS_NFIELDS(clas);
		int i;
		for (i = 0;  i < n;  ++fld, ++i) {
			if (((fld->accflags & ACC_PUBLIC) || declared)
			    && utf8ConstEqualJavaString(fld->name, name)) {
				return makeField(clas, i);
			}
		}
		clas = clas->superclass;
	} while (!declared && clas != NULL);
	SignalError("java.lang.NoSuchFieldException", ""); /* FIXME */
}
