/*
 * debugFile.c
 * Routines for generating an assembly file with debugging information
 *
 * Copyright (c) 2000, 2004 University of Utah and the Flux Group.
 * All rights reserved.
 *
 * This file is licensed under the terms of the GNU Public License.
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * Contributed by the Flux Research Group, Department of Computer Science,
 * University of Utah, http://www.cs.utah.edu/flux/
 */

#include "config.h"

#if defined(KAFFE_XDEBUGGING) || defined(KAFFE_XPROFILER)

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stab.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "kaffe/jmalloc.h"
#include "classMethod.h"
#include "code.h"

#include "mangle.h"
#include "debugFile.h"
#include "xprofiler.h"

struct debug_file *machine_debug_file;
const char *machine_debug_filename = 0;

/**
 * Header to be added to every debug file.
 */
static const char *debug_header = "This file was automatically generated by Kaffe";

/**
 * Builtin types to add to the debugging file.
 */
static const char *types_header =
".stabs \"int:t1=r1;0020000000000;0017777777777;\",128,0,0,0\n"
".stabs \" :t2=*1;\",128,0,0,0\n"
".stabs \"byte:t3=r3;-128;127;\",128,0,0,0\n"
".stabs \" :t4=*3;\",128,0,0,0\n"
".stabs \"short:t5=r5;-32768;32767;\",128,0,0,0\n"
".stabs \" :t6=*5;\",128,0,0,0\n"
".stabs \"jchar:t7=-30;\",128,0,0,0\n"
".stabs \" :t8=*7;\",128,0,0,0\n"
".stabs \"long:t9=r1;01000000000000000000000;0777777777777777777777;\",128,0,0,0\n"
".stabs \" :t10=*9;\",128,0,0,0\n"
".stabs \"float:t11=r1;4;0;\",128,0,0,0\n"
".stabs \" :t12=*11;\",128,0,0,0\n"
".stabs \"double:t13=r1;8;0;\",128,0,0,0\n"
".stabs \" :t14=*13;\",128,0,0,0\n"
".stabs	\"boolean:t15=@s8;eFalse:0,True:1,;\",128,0,0,0\n"
".stabs \" :t16=*15;\",128,0,0,0\n"
".stabs \"void:t17=17\",128,0,0,0\n"
".stabs \" :t18=*17\",128,0,0,0\n"
".stabs \" :t19=*xs_dispatchTable:\",128,0,0,0\n"
".stabs \" :t20=*xs_iLock:\",128,0,0,0\n"
".stabs \"promoted_byte:t21=r21;0020000000000;0017777777777;\",128,0,0,0\n"
".stabs \"promoted_short:t22=r22;0020000000000;0017777777777;\",128,0,0,0\n"
".stabs \"promoted_char:t23=r23;0020000000000;0017777777777;\",128,0,0,0\n"
".stabs \"promoted_boolean:t24=eFalse:0,True:1,;\",128,0,0,0\n";

/**
 * Symbols that represent protection values in stabs.
 */
typedef enum {
	STP_PRIVATE = 0,
	STP_PROTECTED = 1,
	STP_PUBLIC = 2,
} st_prot_t;

struct debug_file *createDebugFile(const char *filename)
{
	struct debug_file *retval = 0;

	assert(filename != NULL);
	
	/* Allocate space for the debug_file struct and filename */
	if( (retval = (struct debug_file *)
	     KMALLOC(sizeof(struct debug_file) + strlen(filename) + 1)) )
	{
		retval->df_filename = (char *)(retval + 1);
		strcpy(retval->df_filename, filename);
		retval->df_current_type_id = STYPE_MAX;
		if( (retval->df_file = fopen(retval->df_filename, "w")) )
		{
			addDebugInfo(retval,
				     DIA_SourceFile, "$xdb$.java", 0,
				     DIA_Comment, debug_header,
				     DIA_DONE);
			fprintf(retval->df_file, "%s", types_header);
		}
		else
		{
			KFREE(retval);
			retval = 0;
		}
	}
	return( retval );
}

void deleteDebugFile(struct debug_file *df)
{
	if( df != NULL )
	{
		/* If there was an error in writing the file remove it */
		if( ferror(df->df_file) )
			remove(df->df_filename);
		fclose(df->df_file);
		KFREE(df);
		df = NULL;
	}
}

/**
 * Convert Java access flags into a stabs protection value.
 *
 * @param af The Java style access flags to convert.
 * @return The stabs protection value that the access flags map to.
 */
static inline st_prot_t acc2prot(accessFlags af)
{
    st_prot_t retval = STP_PUBLIC;
    
    if( af & ACC_PRIVATE )
	retval = STP_PRIVATE;
    else if( af & ACC_PROTECTED )
	retval = STP_PROTECTED;
    else if( af & ACC_PUBLIC )
	retval = STP_PUBLIC;
    return( retval );
}

/**
 * Java name space qualifiers that need to be converted.
 */
static const char *df_quals = "/$";

/**
 * Handle adding a class type.
 *
 * @param df The debug file to add information to.
 * @param cl The class to add to the debugging file.
 */
static void dfHandleClass(struct debug_file *df, struct Hjava_lang_Class *cl)
{
	int lpc;

	assert(df != NULL);
	assert(cl != NULL);

	/* Allocate a type id if necessary. */
	if( cl->stab_id == 0 )
	{
		cl->stab_id = df->df_current_type_id + 1;
		df->df_current_type_id += 2;
	}
	/* Add the structure type, */
	fmanglef(df->df_file,
		 ".stabs \"%t",
		 "/", ".", cl->name->data);
	if( cl->loader != NULL )
	{
	    fmanglef(df->df_file,
		     "_%p",
		     cl->loader);
	}
	fprintf(df->df_file,
		":T%d=s%d",
		cl->stab_id - 1,
		CLASS_FSIZE(cl));
	/* ... fill in the base fields/class, */
	if( cl->superclass == NULL )
	{
		fprintf(df->df_file,
			"vtable:%d,%d,%d;",
			STYPE_DISPATCH_TABLE,
			0,
			sizeof(dispatchTable *) * 8);
		fprintf(df->df_file,
			"_$lock:%d,%d,%d;",
			STYPE_ILOCK,
			sizeof(dispatchTable *) * 8,
			sizeof(iLock *) * 8);
	}
	else
	{
		fprintf(df->df_file,
			"!1,020,%d;",
			getSuperclass(cl)->stab_id - 1);
	}
	/* ... fill in the instance fields, */
	for( lpc = 0; lpc < CLASS_NIFIELDS(cl); lpc++ )
	{
		fields *fld = CLASS_IFIELDS(cl);
		struct Hjava_lang_Class *ftype;
		
		if( (ftype = FIELD_TYPE(&fld[lpc])) != NULL )
		{
			fprintf(df->df_file,
				"%s:/%d%d,%d,%d;",
				fld[lpc].name->data,
				acc2prot(fld[lpc].accflags),
				ftype->stab_id,
				FIELD_BOFFSET(&fld[lpc]) * 8,
				FIELD_SIZE(&fld[lpc]) * 8);
		}
	}
	/* ... the static fields, and */
	for( lpc = 0; lpc < CLASS_NSFIELDS(cl); lpc++ )
	{
		fields *fld = CLASS_SFIELDS(cl);
		struct Hjava_lang_Class *ftype;

		if( (ftype = FIELD_TYPE(&fld[lpc])) != NULL )
		{
			fprintf(df->df_file,
				"%s:/%d%d:",
				fld[lpc].name->data,
				acc2prot(fld[lpc].accflags),
				ftype->stab_id);
			fmanglef(df->df_file,
				 "_ZN%q%d%sE;",
				 df_quals, cl->name->data, cl->loader,
				 strlen(fld[lpc].name->data),
				 fld[lpc].name->data);
		}
	}
	/* ... the synthetic 'class' static member. */
	fmanglef(df->df_file,
		 "class:xsHjava_lang_Class:"
		 ":_ZN%q%d%sE;",
		 df_quals, cl->name->data, cl->loader,
		 strlen("class"),
		 "class");
	fprintf(df->df_file, ";\",%d,0,0,0\n", N_LSYM);
	/* Add a typedef and */
	fmanglef(df->df_file,
		 ".stabs \"%t",
		 "/", ".", cl->name->data);
	if( cl->loader != NULL )
	{
	    fprintf(df->df_file, "_%p", cl->loader);
	}
	fprintf(df->df_file,
		":t%d\",%d,0,0,0\n",
		cl->stab_id - 1,
		N_LSYM);
	/* ... an anonymous pointer type. */
	fprintf(df->df_file,
		".stabs \" :%d=*%d\",%d,0,0,0\n",
		cl->stab_id,
		cl->stab_id - 1,
		N_LSYM);
	
	/* Add symbols and their values. */
	for( lpc = 0; lpc < CLASS_NSFIELDS(cl); lpc++ )
	{
		fields *fld = CLASS_SFIELDS(cl);
		struct Hjava_lang_Class *ftype;

		if( (ftype = FIELD_TYPE(&fld[lpc])) != NULL )
		{
			fmanglef(df->df_file,
				 ".globl _ZN%q%d%sE\n",
				 df_quals, cl->name->data, cl->loader,
				 strlen(fld[lpc].name->data),
				 fld[lpc].name->data);
			
			fmanglef(df->df_file,
				 "_ZN%q%d%sE = %p\n",
				 df_quals, cl->name->data, cl->loader,
				 strlen(fld[lpc].name->data),
				 fld[lpc].name->data,
				 FIELD_ADDRESS(&fld[lpc]));

			fmanglef(df->df_file,
				 ".stabs \"_ZN%q%d%sE:",
				 df_quals, cl->name->data, cl->loader,
				 strlen(fld[lpc].name->data),
				 fld[lpc].name->data);
			fprintf(df->df_file,
				"G%d\",%d,0,0,0\n",
				ftype->stab_id,
				N_GSYM);
		}
	}
	fmanglef(df->df_file,
		 ".globl _ZN%q%d%sE\n",
		 df_quals, cl->name->data, cl->loader,
		 strlen("class"),
		 "class");
	
	fmanglef(df->df_file, "_ZN%q%d%sE = %p\n",
		 df_quals, cl->name->data, cl->loader,
		 strlen("class"),
		 "class",
		 cl);
	
	fmanglef(df->df_file, ".stabs \"_ZN%q%d%sE:",
		 df_quals, cl->name->data, cl->loader,
		 strlen("class"),
		 "class");
	fprintf(df->df_file,
		"GxsHjava_lang_Class:\",%d,0,0,0\n",
		N_GSYM);
}

static void dfHandleArray(struct debug_file *df, struct Hjava_lang_Class *cl)
{
	struct Hjava_lang_Class *etype;

	assert(df != NULL);
	assert(cl != NULL);

	etype = Kaffe_get_array_element_type(cl);
	if( cl->stab_id == 0 )
	{
		cl->stab_id = df->df_current_type_id + 1;
		df->df_current_type_id += 2;
	}
	fprintf(df->df_file,
		".stabs "
		"\" :t%d=*%d=s%dlength:%d,%d,%d;data:ar%d;0;0;%d,%d,0;;\""
		",%d,0,0,0\n",
		cl->stab_id,
		cl->stab_id - 1,
		ARRAY_DATA_OFFSET,
		STYPE_INT,
		ARRAY_SIZE_OFFSET * 8,
		sizeof(int) * 8,
		STYPE_INT,
		etype->stab_id,
		ARRAY_DATA_OFFSET * 8,
		N_LSYM);
}

/**
 * Promote a local variable type, for example, a Java byte becomes an integer.
 *
 * @param st The stab type to promote.
 * @return The promoted type, if the input was a primitive it will be promoted,
 * otherwise the input type.
 */
static inline stype_t promote_stype(stype_t st)
{
    stype_t retval;
    
    switch( st )
    {
    case STYPE_BYTE:
	retval = STYPE_PROMOTED_BYTE;
	break;
    case STYPE_SHORT:
	retval = STYPE_PROMOTED_SHORT;
	break;
    case STYPE_CHAR:
	retval = STYPE_PROMOTED_CHAR;
	break;
    case STYPE_BOOLEAN:
	retval = STYPE_PROMOTED_BOOLEAN;
	break;
    default:
	retval = st;
	break;
    }
    return( retval );
}

/**
 * Handle a local variable.
 *
 * @param df The debug file to add the information to.
 * @param tag Either DIA_Parameter or DIA_LocalVariable.
 * @param name The parameter/local name.
 * @param cl The type of the variable.
 * @param offset The offset of the variable in the stack frame.
 */
static void dfHandleLocalVariable(struct debug_file *df,
				  df_tag_t tag,
				  char *name,
				  struct Hjava_lang_Class *cl,
				  int offset)
{
	assert(df != NULL);
	assert(name != NULL);
	assert(cl != NULL);
	
	fprintf(df->df_file,
		".stabs \"%s:%s%d\",%d,0,0,%d\n",
		name,
		tag == DIA_Parameter ? "p" : "",
		promote_stype(cl->stab_id),
		tag == DIA_Parameter ? N_PSYM : N_LSYM,
		offset);
}

int addDebugInfo(struct debug_file *df, df_tag_t tag, ...)
{
	int retval = 1;
	va_list args;

#if defined(KAFFE_XPROFILER)
	xProfilingOff();
#endif
	va_start(args, tag);
	if( df != NULL )
	{
		lockMutex(df);
		/* Walk over the arguments until we hit the terminator */
		while( tag != DIA_DONE )
		{
			char *str, *name, *addr, *path;
			struct Hjava_lang_Class *cl;
			struct mangled_method *mm;
			int line, size, offset;
			Method *meth;

			switch( tag )
			{
			case DIA_FunctionSymbolS:
				name = va_arg(args, char *);
				addr = va_arg(args, char *);
				size = va_arg(args, int);
				fprintf(df->df_file,
					".weak %s\n"
					"%s = %p\n",
					name,
					name,
					addr);
				if( size > 0 )
				{
					fprintf(df->df_file,
						".weak %s_end\n"
						"%s_end = %p\n",
						name,
						name,
						addr + size);
				}
				break;
			case DIA_FunctionSymbol:
				mm = va_arg(args, struct mangled_method *);
				addr = va_arg(args, char *);
				size = va_arg(args, int);
				fprintf(df->df_file, ".weak ");
				printMangledMethod(mm, df->df_file);
				fprintf(df->df_file, "\n");
				printMangledMethod(mm, df->df_file);
				fprintf(df->df_file, " = %p\n", addr);
				if( size > 0 )
				{
					fprintf(df->df_file, ".weak ");
					printMangledMethod(mm, df->df_file);
					fprintf(df->df_file, "_end\n");
					printMangledMethod(mm, df->df_file);
					fprintf(df->df_file,
						"_end = %p\n",
						addr + size);
				}
				break;
			case DIA_Function:
				meth = va_arg(args, Method *);
				mm = va_arg(args, struct mangled_method *);
				line = va_arg(args, int);
				addr = va_arg(args, char *);
				size = va_arg(args, int);
				/* Add the stabs info to the file */
				fprintf(df->df_file,
					"  /* START %s/%s%s */\n"
					".stabs \"",
					meth->name->data,
					CLASS_CNAME(meth->class),
					METHOD_SIGD(meth));
				printMangledMethod(mm, df->df_file);
				fprintf(df->df_file, ":F\",%d,0,%d,%p\n",
					N_FUN,
					line,
					addr);
				/* Add the symbols to the file */
				fprintf(df->df_file,
					"  /* Symbol: %s/%s%s */\n"
					/* ".weak " */,
					meth->name->data,
					CLASS_CNAME(meth->class),
					METHOD_SIGD(meth));
				// printMangledMethod(mm, df->df_file);
				// fprintf(df->df_file, "\n");
				printMangledMethod(mm, df->df_file);
				fprintf(df->df_file,
					" = %p\n"
					"\t.size ",
					addr);
				printMangledMethod(mm, df->df_file);
				fprintf(df->df_file,
					", %d\n",
					size);
				break;
			case DIA_Symbol:
				name = va_arg(args, char *);
				addr = va_arg(args, char *);
				fprintf(df->df_file, "%s = %p\n", name, addr);
				break;
			case DIA_EndFunction:
				addr = va_arg(args, char *);
				/*
				 * Record the highest seen address so far so we
				 * can report it as the last address for the
				 * $xdb$.java file.
				 */
				if( addr > df->df_high )
					df->df_high = addr;
				/*
				 * Re-add the "$xdb$.java" file name to switch
				 * back so more types can be added.
				 */
				fprintf(df->df_file,
					".stabs \"$xdb$.java\",%d,0,0,%p\n",
					N_SOL,
					df->df_high);
				break;
			case DIA_SourceLine:
				line = va_arg(args, int);
				addr = va_arg(args, char *);
				fprintf(df->df_file,
					".stabn %d,0,%d,%p\n",
					N_SLINE,
					line,
					addr);
				break;
			case DIA_SourceFile:
				name = va_arg(args, char *);
				addr = va_arg(args, char *);
				if (addr != NULL)
					fprintf(df->df_file,
						"\n\n.stabs \"%s\",%d,0,0,%p\n",
						name,
						N_SO,
						addr);
				else
					fprintf(df->df_file,
						"\n\n.stabs \"%s\",%d,0,0,0\n",
						name,
						N_SO);
				break;
			case DIA_IncludeFile:
				path = va_arg(args, char *);
				size = va_arg(args, int);
				name = va_arg(args, char *);
				addr = va_arg(args, char *);
				fmanglef(df->df_file,
					 "\n\n.stabs \"%S%s%S\",%d,0,0,%p\n",
					 path, size,
					 size > 0 ? "/" : "",
					 name, strlen(name),
					 N_SOL,
					 addr);
				break;
			case DIA_Class:
				cl = va_arg(args, struct Hjava_lang_Class *);
				dfHandleClass(df, cl);
				break;
			case DIA_Array:
				cl = va_arg(args, struct Hjava_lang_Class *);
				dfHandleArray(df, cl);
				break;
			case DIA_LeftBrace:
				addr = va_arg(args, char *);
				fprintf(df->df_file,
					".stabn %d,0,0,%p\n",
					N_LBRAC,
					addr);
				break;
			case DIA_RightBrace:
				addr = va_arg(args, char *);
				fprintf(df->df_file,
					".stabn %d,0,0,%p\n",
					N_RBRAC,
					addr);
				break;
			case DIA_LocalVariable:
				name = va_arg(args, char *);
				cl = va_arg(args, struct Hjava_lang_Class *);
				offset = va_arg(args, int);
				dfHandleLocalVariable(df,
						      tag,
						      name,
						      cl,
						      offset);
				break;
			case DIA_Parameter:
				name = va_arg(args, char *);
				cl = va_arg(args, struct Hjava_lang_Class *);
				offset = va_arg(args, int);
				dfHandleLocalVariable(df,
						      tag,
						      name,
						      cl,
						      offset);
				break;
			case DIA_Comment:
				str = va_arg(args, char *);
				fprintf(df->df_file,
					"/* %s */\n",
					str);
				break;
			default:
				assert(0);
				break;
			}
			/* Get the next tag to process */
			tag = va_arg(args, int);
		}
		fflush(df->df_file);
		/* Check for I/O error */
		if( ferror(df->df_file) )
			retval = 0;
		unlockMutex(df);
	}
	va_end(args);
#if defined(KAFFE_XPROFILER)
	xProfilingOn();
#endif
	return( retval );
}

#endif /* KAFFE_XDEBUGGING */
