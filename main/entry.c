
/*
*   Copyright (c) 1996-2002, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for creating tag entries.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include <string.h>
#include <ctype.h>        /* to define isspace () */
#include <errno.h>

#if defined (HAVE_SYS_TYPES_H)
# include <sys/types.h>	  /* to declare off_t on some hosts */
#endif
#if defined (HAVE_TYPES_H)
# include <types.h>       /* to declare off_t on some hosts */
#endif
#if defined (HAVE_UNISTD_H)
# include <unistd.h>      /* to declare close (), ftruncate (), truncate () */
#endif

/*  These header files provide for the functions necessary to do file
 *  truncation.
 */
#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif
#ifdef HAVE_IO_H
# include <io.h>
#endif

#include "debug.h"
#include "entry.h"
#include "field.h"
#include "fmt.h"
#include "kind.h"
#include "main.h"
#include "options.h"
#include "ptag.h"
#include "read.h"
#include "routines.h"
#include "sort.h"
#include "strlist.h"
#include "xtag.h"

/*
*   MACROS
*/
#define includeExtensionFlags()         (Option.tagFileFormat > 1)

/*
 *  Portability defines
 */
#if !defined(HAVE_TRUNCATE) && !defined(HAVE_FTRUNCATE) && !defined(HAVE_CHSIZE)
# define USE_REPLACEMENT_TRUNCATE
#endif

/*  Hack for ridiculous practice of Microsoft Visual C++.
 */
#if defined (WIN32) && defined (_MSC_VER)
# define chsize         _chsize
# define open           _open
# define close          _close
# define O_RDWR         _O_RDWR
#endif


/*  Maintains the state of the tag file.
 */
typedef struct eTagFile {
	char *name;
	char *directory;
	MIO *fp;
	struct sNumTags { unsigned long added, prev; } numTags;
	struct sMax { size_t line, tag; } max;
	struct sEtags {
		char *name;
		MIO *fp;
		size_t byteCount;
	} etags;
	vString *vLine;

	unsigned int cork;
	struct sCorkQueue {
		struct sTagEntryInfo* queue;
		unsigned int length;
		unsigned int count;
	} corkQueue;

	boolean patternCacheValid;
} tagFile;

/*
*   DATA DEFINITIONS
*/

tagFile TagFile = {
    NULL,               /* tag file name */
    NULL,               /* tag file directory (absolute) */
    NULL,               /* file pointer */
    { 0, 0 },           /* numTags */
    { 0, 0 },        /* max */
    { NULL, NULL, 0 },  /* etags */
    NULL,                /* vLine */
    .cork = FALSE,
    .corkQueue = {
	    .queue = NULL,
	    .length = 0,
	    .count  = 0
    },
    .patternCacheValid = FALSE,
};

static boolean TagsToStdout = FALSE;

/*
*   FUNCTION PROTOTYPES
*/
#ifdef NEED_PROTO_TRUNCATE
extern int truncate (const char *path, off_t length);
#endif

#ifdef NEED_PROTO_FTRUNCATE
extern int ftruncate (int fd, off_t length);
#endif

/*
*   FUNCTION DEFINITIONS
*/

extern void freeTagFileResources (void)
{
	if (TagFile.directory != NULL)
		eFree (TagFile.directory);
	vStringDelete (TagFile.vLine);
}

extern const char *tagFileName (void)
{
	return TagFile.name;
}

/*
*   Pseudo tag support
*/

static void abort_if_ferror(MIO *const fp)
{
	if (mio_error (fp))
		error (FATAL | PERROR, "cannot write tag file");
}

static void rememberMaxLengths (const size_t nameLength, const size_t lineLength)
{
	if (nameLength > TagFile.max.tag)
		TagFile.max.tag = nameLength;

	if (lineLength > TagFile.max.line)
		TagFile.max.line = lineLength;
}

extern void writePseudoTag (const struct sPtagDesc *desc,
			    const char *const fileName,
			    const char *const pattern,
			    const char *const parserName)
{
	const int length = parserName

#define OPT(X) ((X)?(X):"")
	  ? mio_printf (TagFile.fp, "%s%s%s%s\t%s\t%s\n",
		     PSEUDO_TAG_PREFIX, desc->name, PSEUDO_TAG_SEPARATOR, parserName,
		     OPT(fileName), OPT(pattern))
	  : mio_printf (TagFile.fp, "%s%s\t%s\t/%s/\n",
		     PSEUDO_TAG_PREFIX, desc->name,
		     OPT(fileName), OPT(pattern));
#undef OPT

	abort_if_ferror (TagFile.fp);

	++TagFile.numTags.added;
	rememberMaxLengths (strlen (desc->name), (size_t) length);
}

static void addCommonPseudoTags (void)
{
	int i;

	for (i = 0; i < PTAG_COUNT; i++)
	{
		if (isPtagCommon (i))
			makePtagIfEnabled (i, NULL);
	}
}

extern void makeFileTag (const char *const fileName)
{
	boolean via_line_directive = (strcmp (fileName, getInputFileName()) != 0);
	xtagType     xtag = XTAG_UNKNOWN;

	if (isXtagEnabled(XTAG_FILE_NAMES))
		xtag = XTAG_FILE_NAMES;
	if (isXtagEnabled(XTAG_FILE_NAMES_WITH_TOTAL_LINES))
		xtag = XTAG_FILE_NAMES_WITH_TOTAL_LINES;

	if (xtag != XTAG_UNKNOWN)
	{
		tagEntryInfo tag;
		kindOption  *kind;

		kind = getInputLanguageFileKind();
		Assert (kind);
		kind->enabled = isXtagEnabled(XTAG_FILE_NAMES);

		/* TODO: you can return here if enabled == FALSE. */

		initTagEntry (&tag, baseFilename (fileName), kind);

		tag.isFileEntry     = TRUE;
		tag.lineNumberEntry = TRUE;
		markTagExtraBit (&tag, xtag);

		if (via_line_directive || (!isXtagEnabled(XTAG_FILE_NAMES_WITH_TOTAL_LINES)))
		{
			tag.lineNumber = 1;
		}
		else
		{
			while (readLineFromInputFile () != NULL)
				;		/* Do nothing */
			tag.lineNumber = getInputLineNumber ();
		}

		makeTagEntry (&tag);
	}
}

static void updateSortedFlag (
		const char *const line, MIO *const fp, MIOPos startOfLine)
{
	const char *const tab = strchr (line, '\t');

	if (tab != NULL)
	{
		const long boolOffset = tab - line + 1;  /* where it should be */

		if (line [boolOffset] == '0'  ||  line [boolOffset] == '1')
		{
			MIOPos nextLine;

			if (mio_getpos (fp, &nextLine) == -1 || mio_getpos (fp, &startOfLine) == -1)
				error (WARNING, "Failed to update 'sorted' pseudo-tag");
			else
			{
				MIOPos flagLocation;
				int c, d;

				do
					c = mio_getc (fp);
				while (c != '\t'  &&  c != '\n');
				mio_getpos (fp, &flagLocation);
				d = mio_getc (fp);
				if (c == '\t'  &&  (d == '0'  ||  d == '1')  &&
					d != (int) Option.sorted)
				{
					mio_setpos (fp, &flagLocation);
					mio_putc (fp, Option.sorted == SO_FOLDSORTED ? '2' :
						(Option.sorted == SO_SORTED ? '1' : '0'));
				}
				mio_setpos (fp, &nextLine);
			}
		}
	}
}

/*  Look through all line beginning with "!_TAG_FILE", and update those which
 *  require it.
 */
static long unsigned int updatePseudoTags (MIO *const fp)
{
	enum { maxEntryLength = 20 };
	char entry [maxEntryLength + 1];
	unsigned long linesRead = 0;
	MIOPos startOfLine;
	size_t entryLength;
	const char *line;

	sprintf (entry, "%sTAG_FILE", PSEUDO_TAG_PREFIX);
	entryLength = strlen (entry);
	Assert (entryLength < maxEntryLength);

	mio_getpos (fp, &startOfLine);
	line = readLineRaw (TagFile.vLine, fp);
	while (line != NULL  &&  line [0] == entry [0])
	{
		++linesRead;
		if (strncmp (line, entry, entryLength) == 0)
		{
			char tab, classType [16];

			if (sscanf (line + entryLength, "%15s%c", classType, &tab) == 2  &&
				tab == '\t')
			{
				if (strcmp (classType, "_SORTED") == 0)
					updateSortedFlag (line, fp, startOfLine);
			}
			mio_getpos (fp, &startOfLine);
		}
		line = readLineRaw (TagFile.vLine, fp);
	}
	while (line != NULL)  /* skip to end of file */
	{
		++linesRead;
		line = readLineRaw (TagFile.vLine, fp);
	}
	return linesRead;
}

/*
 *  Tag file management
 */

static boolean isValidTagAddress (const char *const excmd)
{
	boolean isValid = FALSE;

	if (strchr ("/?", excmd [0]) != NULL)
		isValid = TRUE;
	else
	{
		char *address = xMalloc (strlen (excmd) + 1, char);
		if (sscanf (excmd, "%[^;\n]", address) == 1  &&
			strspn (address,"0123456789") == strlen (address))
				isValid = TRUE;
		eFree (address);
	}
	return isValid;
}

static boolean isCtagsLine (const char *const line)
{
	enum fieldList { TAG, TAB1, SRC_FILE, TAB2, EXCMD, NUM_FIELDS };
	boolean ok = FALSE;  /* we assume not unless confirmed */
	const size_t fieldLength = strlen (line) + 1;
	char *const fields = xMalloc (NUM_FIELDS * fieldLength, char);

	if (fields == NULL)
		error (FATAL, "Cannot analyze tag file");
	else
	{
#define field(x)		(fields + ((size_t) (x) * fieldLength))

		const int numFields = sscanf (
			line, "%[^\t]%[\t]%[^\t]%[\t]%[^\r\n]",
			field (TAG), field (TAB1), field (SRC_FILE),
			field (TAB2), field (EXCMD));

		/*  There must be exactly five fields: two tab fields containing
		 *  exactly one tab each, the tag must not begin with "#", and the
		 *  file name should not end with ";", and the excmd must be
		 *  acceptable.
		 *
		 *  These conditions will reject tag-looking lines like:
		 *      int a;        <C-comment>
		 *      #define LABEL <C-comment>
		 */
		if (numFields == NUM_FIELDS   &&
			strlen (field (TAB1)) == 1  &&
			strlen (field (TAB2)) == 1  &&
			field (TAG) [0] != '#'      &&
			field (SRC_FILE) [strlen (field (SRC_FILE)) - 1] != ';'  &&
			isValidTagAddress (field (EXCMD)))
				ok = TRUE;

		eFree (fields);
	}
	return ok;
}

static boolean isEtagsLine (const char *const line)
{
	boolean result = FALSE;
	if (line [0] == '\f')
		result = (boolean) (line [1] == '\n'  ||  line [1] == '\r');
	return result;
}

static boolean isTagFile (const char *const filename)
{
	boolean ok = FALSE;  /* we assume not unless confirmed */
	MIO *const fp = mio_new_file (filename, "rb");

	if (fp == NULL  &&  errno == ENOENT)
		ok = TRUE;
	else if (fp != NULL)
	{
		const char *line = readLineRaw (TagFile.vLine, fp);

		if (line == NULL)
			ok = TRUE;
		else
			ok = (boolean) (isCtagsLine (line) || isEtagsLine (line));
		mio_free (fp);
	}
	return ok;
}

extern void openTagFile (void)
{
	setDefaultTagFileName ();
	TagsToStdout = isDestinationStdout ();

	if (TagFile.vLine == NULL)
		TagFile.vLine = vStringNew ();

	/*  Open the tags file.
	 */
	if (TagsToStdout)
	{
		/* Open a tempfile with read and write mode. Read mode is used when
		 * write the result to stdout. */
		TagFile.fp = tempFile ("w+", &TagFile.name);
		if (isXtagEnabled (XTAG_PSEUDO_TAGS))
			addCommonPseudoTags ();
	}
	else
	{
		boolean fileExists;

		TagFile.name = eStrdup (Option.tagFileName);
		fileExists = doesFileExist (TagFile.name);
		if (fileExists  &&  ! isTagFile (TagFile.name))
			error (FATAL,
			  "\"%s\" doesn't look like a tag file; I refuse to overwrite it.",
				  TagFile.name);

		if (Option.etags)
		{
			if (Option.append  &&  fileExists)
				TagFile.fp = mio_new_file (TagFile.name, "a+b");
			else
				TagFile.fp = mio_new_file (TagFile.name, "w+b");
		}
		else
		{
			if (Option.append  &&  fileExists)
			{
				TagFile.fp = mio_new_file (TagFile.name, "r+");
				if (TagFile.fp != NULL)
				{
					TagFile.numTags.prev = updatePseudoTags (TagFile.fp);
					mio_free (TagFile.fp);
					TagFile.fp = mio_new_file (TagFile.name, "a+");
				}
			}
			else
			{
				TagFile.fp = mio_new_file (TagFile.name, "w");
				if (TagFile.fp != NULL && isXtagEnabled (XTAG_PSEUDO_TAGS))
					addCommonPseudoTags ();
			}
		}
		if (TagFile.fp == NULL)
			error (FATAL | PERROR, "cannot open tag file");
	}
	if (TagsToStdout)
		TagFile.directory = eStrdup (CurrentDirectory);
	else
		TagFile.directory = absoluteDirname (TagFile.name);
}

#ifdef USE_REPLACEMENT_TRUNCATE

static void copyBytes (MIO* const fromFp, MIO* const toFp, const long size)
{
	enum { BufferSize = 1000 };
	long toRead, numRead;
	char* buffer = xMalloc (BufferSize, char);
	long remaining = size;
	do
	{
		toRead = (0 < remaining && remaining < BufferSize) ?
					remaining : (long) BufferSize;
		numRead = mio_read (fromFp, buffer, (size_t) 1, (size_t) toRead);
		if (mio_write (toFp, buffer, (size_t)1, (size_t)numRead) < (size_t)numRead)
			error (FATAL | PERROR, "cannot complete write");
		if (remaining > 0)
			remaining -= numRead;
	} while (numRead == toRead  &&  remaining != 0);
	eFree (buffer);
}

static void copyFile (const char *const from, const char *const to, const long size)
{
	MIO* const fromFp = mio_new_file (from, "rb");
	if (fromFp == NULL)
		error (FATAL | PERROR, "cannot open file to copy");
	else
	{
		MIO* const toFp = mio_new_file (to, "wb");
		if (toFp == NULL)
			error (FATAL | PERROR, "cannot open copy destination");
		else
		{
			copyBytes (fromFp, toFp, size);
			mio_free (toFp);
		}
		mio_free (fromFp);
	}
}

/*  Replacement for missing library function.
 */
static int replacementTruncate (const char *const name, const long size)
{
	char *tempName = NULL;
	MIO *fp = tempFile ("w", &tempName);
	mio_free (fp);
	copyFile (name, tempName, size);
	copyFile (tempName, name, WHOLE_FILE);
	remove (tempName);
	eFree (tempName);

	return 0;
}

#endif

#ifndef EXTERNAL_SORT
static void internalSortTagFile (void)
{
	MIO *fp;

	/*  Open/Prepare the tag file and place its lines into allocated buffers.
	 */
	if (TagsToStdout)
	{
		fp = TagFile.fp;
		mio_seek (fp, 0, SEEK_SET);
	}
	else
	{
		fp = mio_new_file (tagFileName (), "r");
		if (fp == NULL)
			failedSort (fp, NULL);
	}

	internalSortTags (TagsToStdout,
			  fp,
			  TagFile.numTags.added + TagFile.numTags.prev);

	if (! TagsToStdout)
		mio_free (fp);
}
#endif

static void sortTagFile (void)
{
	if (TagFile.numTags.added > 0L)
	{
		if (Option.sorted != SO_UNSORTED)
		{
			verbose ("sorting tag file\n");
#ifdef EXTERNAL_SORT
			externalSortTags (TagsToStdout, TagFile.fp);
#else
			internalSortTagFile ();
#endif
		}
		else if (TagsToStdout)
			catFile (TagFile.fp);
	}
}

static void resizeTagFile (const long newSize)
{
	int result;

#ifdef USE_REPLACEMENT_TRUNCATE
	result = replacementTruncate (TagFile.name, newSize);
#else
# ifdef HAVE_TRUNCATE
	result = truncate (TagFile.name, (off_t) newSize);
# else
	const int fd = open (TagFile.name, O_RDWR);

	if (fd == -1)
		result = -1;
	else
	{
#  ifdef HAVE_FTRUNCATE
		result = ftruncate (fd, (off_t) newSize);
#  else
#   ifdef HAVE_CHSIZE
		result = chsize (fd, newSize);
#   endif
#  endif
		close (fd);
	}
# endif
#endif
	if (result == -1)
		fprintf (stderr, "Cannot shorten tag file: errno = %d\n", errno);
}

static void writeEtagsIncludes (MIO *const fp)
{
	if (Option.etagsInclude)
	{
		unsigned int i;
		for (i = 0  ;  i < stringListCount (Option.etagsInclude)  ;  ++i)
		{
			vString *item = stringListItem (Option.etagsInclude, i);
			mio_printf (fp, "\f\n%s,include\n", vStringValue (item));
		}
	}
}

extern void closeTagFile (const boolean resize)
{
	long desiredSize, size;

	if (Option.etags)
		writeEtagsIncludes (TagFile.fp);
	mio_flush (TagFile.fp);
	abort_if_ferror (TagFile.fp);
	desiredSize = mio_tell (TagFile.fp);
	mio_seek (TagFile.fp, 0L, SEEK_END);
	size = mio_tell (TagFile.fp);
	if (! TagsToStdout)
		/* The tag file should be closed before resizing. */
		if (mio_free (TagFile.fp) != 0)
			error (FATAL | PERROR, "cannot close tag file");

	if (resize  &&  desiredSize < size)
	{
		DebugStatement (
			debugPrintf (DEBUG_STATUS, "shrinking %s from %ld to %ld bytes\n",
				TagFile.name, size, desiredSize); )
		resizeTagFile (desiredSize);
	}
	sortTagFile ();
	if (TagsToStdout)
	{
		if (mio_free (TagFile.fp) != 0)
			error (FATAL | PERROR, "cannot close tag file");
		remove (tagFileName ());  /* remove temporary file */
	}
	eFree (TagFile.name);
	TagFile.name = NULL;
}

extern void beginEtagsFile (void)
{
	TagFile.etags.fp = tempFile ("w+b", &TagFile.etags.name);
	TagFile.etags.byteCount = 0;
}

extern void endEtagsFile (const char *const filename)
{
	const char *line;

	mio_printf (TagFile.fp, "\f\n%s,%ld\n", filename, (long) TagFile.etags.byteCount);
	abort_if_ferror (TagFile.fp);

	if (TagFile.etags.fp != NULL)
	{
		mio_rewind (TagFile.etags.fp);
		while ((line = readLineRaw (TagFile.vLine, TagFile.etags.fp)) != NULL)
			mio_puts (TagFile.fp, line);
		mio_free (TagFile.etags.fp);
		remove (TagFile.etags.name);
		eFree (TagFile.etags.name);
		TagFile.etags.fp = NULL;
		TagFile.etags.name = NULL;
	}
}

/*
 *  Tag entry management
 */

/*  This function copies the current line out to a specified file. It has no
 *  effect on the fileGetc () function.  During copying, any '\' characters
 *  are doubled and a leading '^' or trailing '$' is also quoted. End of line
 *  characters (line feed or carriage return) are dropped.
 */
static size_t appendInputLine (int putc_func (char , void *), const char *const line, void * data, boolean *omitted)
{
	size_t length = 0;
	const char *p;

	/*  Write everything up to, but not including, a line end character.
	 */
	*omitted = FALSE;
	for (p = line  ;  *p != '\0'  ;  ++p)
	{
		const int next = *(p + 1);
		const int c = *p;

		if (c == CRETURN  ||  c == NEWLINE)
			break;

		if (length >= Option.patternLengthLimit)
		{
			*omitted = TRUE;
			break;
		}
		/*  If character is '\', or a terminal '$', then quote it.
		 */
		if (c == BACKSLASH  ||  c == (Option.backward ? '?' : '/')  ||
			(c == '$'  &&  (next == NEWLINE  ||  next == CRETURN)))
		{
			putc_func (BACKSLASH, data);
			++length;
		}
		putc_func (c, data);
		++length;
	}

	return length;
}

static int vstring_putc (char c, void *data)
{
	vString *str = data;
	vStringPut (str, c);
	return 1;
}

static int vstring_puts (const char* s, void *data)
{
	vString *str = data;
	int len = vStringLength (str);
	vStringCatS (str, s);
	return vStringLength (str) - len;
}

static int file_putc (char c, void *data)
{
	MIO *fp = data;
	mio_putc (fp, c);
	return 1;
}

static int file_puts (const char* s, void *data)
{
	MIO *fp = data;
	return mio_puts (fp, s);
}

static boolean isPosSet(MIOPos pos)
{
	char * p = (char *)&pos;
	boolean r = FALSE;
	unsigned int i;

	for (i = 0; i < sizeof(pos); i++)
		r |= p[i];
	return r;
}

extern char *readLineFromBypassAnyway (vString *const vLine, const tagEntryInfo *const tag,
				   long *const pSeekValue)
{
	char * line;

	if (isPosSet (tag->filePosition) || (tag->pattern == NULL))
		line = 	readLineFromBypass (vLine, tag->filePosition, pSeekValue);
	else
		line = readLineFromBypassSlow (vLine, tag->lineNumber, tag->pattern, pSeekValue);

	return line;
}

static const char* escapeName (const tagEntryInfo * tag, fieldType ftype)
{
	fieldDesc *fdesc = getFieldDesc (ftype);
	return renderFieldEscaped (fdesc, tag);
}

static int writeXrefEntry (const tagEntryInfo *const tag)
{
	int length;
	static fmtElement *fmt1;
	static fmtElement *fmt2;

	if (Option.customXfmt)
		length = fmtPrint (Option.customXfmt, TagFile.fp, tag);
	else
	{
		if (tag->isFileEntry)
			return 0;

		if (Option.tagFileFormat == 1)
		{
			if (fmt1 == NULL)
				fmt1 = fmtNew ("%-16N %4n %-16F %C");
			length = fmtPrint (fmt1, TagFile.fp, tag);
		}
		else
		{
			if (fmt2 == NULL)
				fmt2 = fmtNew ("%-16N %-10K %4n %-16F %C");
			length = fmtPrint (fmt2, TagFile.fp, tag);
		}
	}

	mio_putc (TagFile.fp, '\n');
	length++;

	return length;
}

/*  Truncates the text line containing the tag at the character following the
 *  tag, providing a character which designates the end of the tag.
 */
static void truncateTagLine (
		char *const line, const char *const token, const boolean discardNewline)
{
	char *p = strstr (line, token);

	if (p != NULL)
	{
		p += strlen (token);
		if (*p != '\0'  &&  ! (*p == '\n'  &&  discardNewline))
			++p;    /* skip past character terminating character */
		*p = '\0';
	}
}

static int writeEtagsEntry (const tagEntryInfo *const tag)
{
	int length;

	if (tag->isFileEntry)
		length = mio_printf (TagFile.etags.fp, "\177%s\001%lu,0\n",
				tag->name, tag->lineNumber);
	else
	{
		long seekValue;
		char *const line =
				readLineFromBypassAnyway (TagFile.vLine, tag, &seekValue);
		if (line == NULL)
			return 0;

		if (tag->truncateLine)
			truncateTagLine (line, tag->name, TRUE);
		else
			line [strlen (line) - 1] = '\0';

		length = mio_printf (TagFile.etags.fp, "%s\177%s\001%lu,%ld\n", line,
				tag->name, tag->lineNumber, seekValue);
	}
	TagFile.etags.byteCount += length;

	return length;
}

static char* getFullQualifiedScopeNameFromCorkQueue (const tagEntryInfo * inner_scope)
{

	const kindOption *kind = NULL;
	const tagEntryInfo *scope = inner_scope;
	stringList *queue = stringListNew ();
	vString *v;
	vString *n;
	unsigned int c;
	const char *sep;

	while (scope)
	{
		if (!scope->placeholder)
		{
			if (kind)
			{
				sep = scopeSeparatorFor (kind, scope->kind->letter);
				v = vStringNewInit (sep);
				stringListAdd (queue, v);
			}
			v = vStringNewInit (escapeName (scope, FIELD_NAME));
			stringListAdd (queue, v);
			kind = scope->kind;
		}
		scope =  getEntryInCorkQueue (scope->extensionFields.scopeIndex);
	}

	n = vStringNew ();
	while ((c = stringListCount (queue)) > 0)
	{
		v = stringListLast (queue);
		vStringCat (n, v);
		vStringDelete (v);
		stringListRemoveLast (queue);
	}
	stringListDelete (queue);

	return vStringDeleteUnwrap (n);
}

static int addExtensionFields (const tagEntryInfo *const tag)
{
	const char* const kindKey = getFieldDesc (FIELD_KIND_KEY)->enabled
		?getFieldName (FIELD_KIND_KEY)
		:"";
	const char* const kindFmt = getFieldDesc (FIELD_KIND_KEY)->enabled
		?"%s\t%s:%s"
		:"%s\t%s%s";
	const char* const scopeKey = getFieldDesc (FIELD_SCOPE_KEY)->enabled
		?getFieldName (FIELD_SCOPE_KEY)
		:"";
	const char* const scopeFmt = getFieldDesc (FIELD_SCOPE_KEY)->enabled
		?"%s\t%s:%s:%s"
		:"%s\t%s%s:%s";

	boolean first = TRUE;
	const char* separator = ";\"";
	const char* const empty = "";
	int length = 0;
/* "sep" returns a value only the first time it is evaluated */
#define sep (first ? (first = FALSE, separator) : empty)

	if (tag->kind->name != NULL && (getFieldDesc (FIELD_KIND_LONG)->enabled  ||
		 (getFieldDesc (FIELD_KIND)->enabled  && tag->kind == '\0')))
		length += mio_printf (TagFile.fp, kindFmt, sep, kindKey, tag->kind->name);
	else if (tag->kind != '\0'  && (getFieldDesc (FIELD_KIND)->enabled ||
			(getFieldDesc (FIELD_KIND_LONG)->enabled &&  tag->kind->name == NULL)))
	{
		char str[2] = {tag->kind->letter, '\0'};
		length += mio_printf (TagFile.fp, kindFmt, sep, kindKey, str);
	}

	if (getFieldDesc (FIELD_LINE_NUMBER)->enabled)
		length += mio_printf (TagFile.fp, "%s\t%s:%ld", sep,
				   getFieldName (FIELD_LINE_NUMBER),
				   tag->lineNumber);

	if (getFieldDesc (FIELD_LANGUAGE)->enabled  &&  tag->language != NULL)
		length += mio_printf (TagFile.fp, "%s\t%s:%s", sep,
				   getFieldName (FIELD_LANGUAGE),
				   escapeName (tag, FIELD_LANGUAGE));

	if (getFieldDesc (FIELD_SCOPE)->enabled)
	{
		if (tag->extensionFields.scopeKind != NULL  &&
		    tag->extensionFields.scopeName != NULL)
			length += mio_printf (TagFile.fp, scopeFmt, sep,
					   scopeKey,
					   tag->extensionFields.scopeKind->name,
					   escapeName (tag, FIELD_SCOPE));
		else if (tag->extensionFields.scopeIndex != SCOPE_NIL
			 && TagFile.corkQueue.count > 0)
		{
			const tagEntryInfo * scope;
			char *full_qualified_scope_name;

			scope = getEntryInCorkQueue (tag->extensionFields.scopeIndex);
			full_qualified_scope_name = getFullQualifiedScopeNameFromCorkQueue(scope);
			Assert (full_qualified_scope_name);
			length += mio_printf (TagFile.fp, scopeFmt, sep,
					   scopeKey,
					   scope->kind->name, full_qualified_scope_name);

			/* TODO: Make the value pointed by full_qualified_scope_name reusable. */
			eFree (full_qualified_scope_name);
		}
	}

	if (getFieldDesc (FIELD_TYPE_REF)->enabled &&
			tag->extensionFields.typeRef [0] != NULL  &&
			tag->extensionFields.typeRef [1] != NULL)
		length += mio_printf (TagFile.fp, "%s\t%s:%s:%s", sep,
				   getFieldName (FIELD_TYPE_REF),
				   tag->extensionFields.typeRef [0],
				   escapeName (tag, FIELD_TYPE_REF));

	if (getFieldDesc (FIELD_FILE_SCOPE)->enabled &&  tag->isFileScope)
		length += mio_printf (TagFile.fp, "%s\t%s:", sep,
				   getFieldName (FIELD_FILE_SCOPE));

	if (getFieldDesc (FIELD_INHERITANCE)->enabled &&
			tag->extensionFields.inheritance != NULL)
		length += mio_printf (TagFile.fp, "%s\t%s:%s", sep,
				   getFieldName (FIELD_INHERITANCE),
				   escapeName (tag, FIELD_INHERITANCE));

	if (getFieldDesc (FIELD_ACCESS)->enabled &&  tag->extensionFields.access != NULL)
		length += mio_printf (TagFile.fp, "%s\t%s:%s", sep,
				   getFieldName (FIELD_ACCESS),
				   tag->extensionFields.access);

	if (getFieldDesc (FIELD_IMPLEMENTATION)->enabled &&
			tag->extensionFields.implementation != NULL)
		length += mio_printf (TagFile.fp, "%s\t%s:%s", sep,
				   getFieldName (FIELD_IMPLEMENTATION),
				   tag->extensionFields.implementation);

	if (getFieldDesc (FIELD_SIGNATURE)->enabled &&
			tag->extensionFields.signature != NULL)
		length += mio_printf (TagFile.fp, "%s\t%s:%s", sep,
				   getFieldName (FIELD_SIGNATURE),
				   escapeName (tag, FIELD_SIGNATURE));
	if (getFieldDesc (FIELD_ROLE)->enabled && tag->extensionFields.roleIndex != ROLE_INDEX_DEFINITION)
		length += mio_printf (TagFile.fp, "%s\t%s:%s", sep,
				   getFieldName (FIELD_ROLE),
				   escapeName (tag, FIELD_ROLE));

	if (getFieldDesc (FIELD_EXTRA)->enabled)
	{
		const char *value = escapeName (tag, FIELD_EXTRA);
		if (value)
			length += mio_printf (TagFile.fp, "%s\t%s:%s", sep,
					   getFieldName (FIELD_EXTRA),
					   escapeName (tag, FIELD_EXTRA));
	}

	return length;
#undef sep
}

static int   makePatternStringCommon (const tagEntryInfo *const tag,
				      int putc_func (char , void *),
				      int puts_func (const char* , void *),
				      void *output)
{
	int length = 0;

	char *line;
	int searchChar;
	const char *terminator;
	boolean  omitted;
	size_t line_len;

	boolean making_cache = FALSE;
	int (* puts_o_func)(const char* , void *);
	void * o_output;

	static vString *cached_pattern;
	static MIOPos   cached_location;
	if (TagFile.patternCacheValid
	    && (! tag->truncateLine)
	    && (memcmp (&tag->filePosition, &cached_location, sizeof(MIOPos)) == 0))
		return puts_func (vStringValue (cached_pattern), output);

	line = readLineFromBypass (TagFile.vLine, tag->filePosition, NULL);
	if (line == NULL)
		error (FATAL, "could not read tag line from %s at line %lu", getInputFileName (),tag->lineNumber);
	if (tag->truncateLine)
		truncateTagLine (line, tag->name, FALSE);

	line_len = strlen (line);
	searchChar = Option.backward ? '?' : '/';
	terminator = (boolean) (line [line_len - 1] == '\n') ? "$": "";

	if (!tag->truncateLine)
	{
		making_cache = TRUE;
		if (cached_pattern == NULL)
			cached_pattern = vStringNew();
		else
			vStringClear (cached_pattern);

		puts_o_func = puts_func;
		o_output    = output;
		putc_func   = vstring_putc;
		puts_func   = vstring_puts;
		output      = cached_pattern;
	}

	length += putc_func(searchChar, output);
	length += putc_func('^', output);
	length += appendInputLine (putc_func, line, output, &omitted);
	length += puts_func (omitted? "": terminator, output);
	length += putc_func (searchChar, output);

	if (making_cache)
	{
		puts_o_func (vStringValue (cached_pattern), o_output);
		cached_location = tag->filePosition;
		TagFile.patternCacheValid = TRUE;
	}

	return length;
}

extern char* makePatternString (const tagEntryInfo *const tag)
{
	vString* pattern = vStringNew ();
	makePatternStringCommon (tag, vstring_putc, vstring_puts, pattern);
	return vStringDeleteUnwrap (pattern);
}

static int writePatternEntry (const tagEntryInfo *const tag)
{
	return makePatternStringCommon (tag, file_putc, file_puts, TagFile.fp);
}

static int writeLineNumberEntry (const tagEntryInfo *const tag)
{
	if (Option.lineDirectives)
		return mio_printf (TagFile.fp, "%s", escapeName (tag, FIELD_LINE_NUMBER));
	else
		return mio_printf (TagFile.fp, "%lu", tag->lineNumber);
}

static int writeCtagsEntry (const tagEntryInfo *const tag)
{
	int length = mio_printf (TagFile.fp, "%s\t%s\t",
			      escapeName (tag, FIELD_NAME),
			      escapeName (tag, FIELD_INPUT_FILE));

	if (tag->lineNumberEntry)
		length += writeLineNumberEntry (tag);
	else if (tag->pattern)
		length += mio_printf(TagFile.fp, "%s", tag->pattern);
	else
		length += writePatternEntry (tag);

	if (includeExtensionFlags ())
		length += addExtensionFields (tag);

	length += mio_printf (TagFile.fp, "\n");

	return length;
}

static void recordTagEntryInQueue (const tagEntryInfo *const tag, tagEntryInfo* slot)
{
	*slot = *tag;

	if (slot->pattern)
		slot->pattern = eStrdup (slot->pattern);
	else if (!slot->lineNumberEntry)
		slot->pattern = makePatternString (slot);

	slot->inputFileName = eStrdup (slot->inputFileName);
	slot->name = eStrdup (slot->name);
	if (slot->extensionFields.access)
		slot->extensionFields.access = eStrdup (slot->extensionFields.access);
	if (slot->extensionFields.fileScope)
		slot->extensionFields.fileScope = eStrdup (slot->extensionFields.fileScope);
	if (slot->extensionFields.implementation)
		slot->extensionFields.implementation = eStrdup (slot->extensionFields.implementation);
	if (slot->extensionFields.inheritance)
		slot->extensionFields.inheritance = eStrdup (slot->extensionFields.inheritance);
	if (slot->extensionFields.scopeName)
		slot->extensionFields.scopeName = eStrdup (slot->extensionFields.scopeName);
	if (slot->extensionFields.signature)
		slot->extensionFields.signature = eStrdup (slot->extensionFields.signature);
	if (slot->extensionFields.typeRef[0])
		slot->extensionFields.typeRef[0] = eStrdup (slot->extensionFields.typeRef[0]);
	if (slot->extensionFields.typeRef[1])
		slot->extensionFields.typeRef[1] = eStrdup (slot->extensionFields.typeRef[1]);

	if (slot->sourceLanguage)
		slot->sourceLanguage = eStrdup (slot->sourceLanguage);
	if (slot->sourceFileName)
		slot->sourceFileName = eStrdup (slot->sourceFileName);
}

static void clearTagEntryInQueue (tagEntryInfo* slot)
{
	if (slot->pattern)
		eFree ((char *)slot->pattern);
	eFree ((char *)slot->inputFileName);
	eFree ((char *)slot->name);

	if (slot->extensionFields.access)
		eFree ((char *)slot->extensionFields.access);
	if (slot->extensionFields.fileScope)
		eFree ((char *)slot->extensionFields.fileScope);
	if (slot->extensionFields.implementation)
		eFree ((char *)slot->extensionFields.implementation);
	if (slot->extensionFields.inheritance)
		eFree ((char *)slot->extensionFields.inheritance);
	if (slot->extensionFields.scopeKind)
		slot->extensionFields.scopeKind = NULL;
	if (slot->extensionFields.scopeName)
		eFree ((char *)slot->extensionFields.scopeName);
	if (slot->extensionFields.signature)
		eFree ((char *)slot->extensionFields.signature);
	if (slot->extensionFields.typeRef[0])
		eFree ((char *)slot->extensionFields.typeRef[0]);
	if (slot->extensionFields.typeRef[1])
		eFree ((char *)slot->extensionFields.typeRef[1]);

	if (slot->sourceLanguage)
		eFree ((char *)slot->sourceLanguage);
	if (slot->sourceFileName)
		eFree ((char *)slot->sourceFileName);
}


static unsigned int queueTagEntry(const tagEntryInfo *const tag)
{
	unsigned int i;
	void *tmp;
	tagEntryInfo * slot;

	if (! (TagFile.corkQueue.count < TagFile.corkQueue.length))
	{
		if (!TagFile.corkQueue.length)
			TagFile.corkQueue.length = 1;

		tmp = eRealloc (TagFile.corkQueue.queue,
				sizeof (*TagFile.corkQueue.queue) * TagFile.corkQueue.length * 2);

		TagFile.corkQueue.length *= 2;
		TagFile.corkQueue.queue = tmp;
	}

	i = TagFile.corkQueue.count;
	TagFile.corkQueue.count++;


	slot = TagFile.corkQueue.queue + i;
	recordTagEntryInQueue (tag, slot);

	return i;
}

static void writeTagEntry (const tagEntryInfo *const tag)
{
	int length = 0;

	if (tag->placeholder)
		return;

	DebugStatement ( debugEntry (tag); )
	if (Option.xref)
		length = writeXrefEntry (tag);
	else if (Option.etags)
		length = writeEtagsEntry (tag);
	else
		length = writeCtagsEntry (tag);

	++TagFile.numTags.added;
	rememberMaxLengths (strlen (tag->name), (size_t) length);
	DebugStatement ( mio_flush (TagFile.fp); )

	abort_if_ferror (TagFile.fp);
}

extern void corkTagFile(void)
{
	TagFile.cork++;
	if (TagFile.cork == 1)
	{
		  TagFile.corkQueue.length = 1;
		  TagFile.corkQueue.count = 1;
		  TagFile.corkQueue.queue = eMalloc (sizeof (*TagFile.corkQueue.queue));
		  memset (TagFile.corkQueue.queue, 0, sizeof (*TagFile.corkQueue.queue));
	}
}

extern void uncorkTagFile(void)
{
	unsigned int i;

	TagFile.cork--;

	if (TagFile.cork > 0)
		return ;

	for (i = 1; i < TagFile.corkQueue.count; i++)
		writeTagEntry (TagFile.corkQueue.queue + i);
	for (i = 1; i < TagFile.corkQueue.count; i++)
		clearTagEntryInQueue (TagFile.corkQueue.queue + i);

	memset (TagFile.corkQueue.queue, 0,
		sizeof (*TagFile.corkQueue.queue) * TagFile.corkQueue.count);
	TagFile.corkQueue.count = 0;
	eFree (TagFile.corkQueue.queue);
	TagFile.corkQueue.queue = NULL;
	TagFile.corkQueue.length = 0;
}

extern tagEntryInfo *getEntryInCorkQueue   (unsigned int n)
{
	if ((SCOPE_NIL < n) && (n < TagFile.corkQueue.count))
		return TagFile.corkQueue.queue + n;
	else
		return NULL;
}

extern size_t        countEntryInCorkQueue (void)
{
	return TagFile.corkQueue.count;
}

extern int makeTagEntry (const tagEntryInfo *const tag)
{
	int r = SCOPE_NIL;
	Assert (tag->name != NULL);
	Assert (getInputLanguageFileKind() == tag->kind
		|| ( isInputLanguageKindEnabled (tag->kind->letter)
                    && (tag->extensionFields.roleIndex == ROLE_INDEX_DEFINITION ))
		|| (tag->extensionFields.roleIndex != ROLE_INDEX_DEFINITION
		    && tag->kind->roles[tag->extensionFields.roleIndex].enabled ));

	if (tag->name [0] == '\0' && (!tag->placeholder))
	{
		if (!doesInputLanguageAllowNullTag())
			error (WARNING, "ignoring null tag in %s(line: %lu)",
			       getInputFileName (), tag->lineNumber);
		goto out;
	}

	if (TagFile.cork)
		r = queueTagEntry (tag);
	else
		writeTagEntry (tag);
out:
	return r;
}

extern int makeQualifiedTagEntry (const tagEntryInfo *const e)
{
	int r = SCOPE_NIL;
	tagEntryInfo x;
	char xk;
	const char *sep;
	static vString *fqn;

	if (isXtagEnabled (XTAG_QUALIFIED_TAGS))
	{
		x = *e;
		markTagExtraBit (&x, XTAG_QUALIFIED_TAGS);

		if (fqn == NULL)
			fqn = vStringNew ();
		else
			vStringClear (fqn);

		if (e->extensionFields.scopeName)
		{
			vStringCatS (fqn, e->extensionFields.scopeName);
			xk = e->extensionFields.scopeKind->letter;
			sep = scopeSeparatorFor (e->kind, xk);
			vStringCatS (fqn, sep);
		}
		else
		{
			/* This is an top level item. prepend a root separator
			   if the kind of the item has. */
			sep = scopeSeparatorFor (e->kind, KIND_NULL);
			if (sep == NULL)
			{
				/* No root separator. The name of the
				   oritinal tag and that of full qualified tag
				   are the same; recording the full qualified tag
				   is meaningless. */
				return r;
			}
			else
				vStringCatS (fqn, sep);
		}
		vStringCatS (fqn, e->name);

		x.name = vStringValue (fqn);
		/* makeExtraTagEntry of c.c doesn't clear scope
		   releated fields. */
#if 0
		x.extensionFields.scopeKind = NULL;
		x.extensionFields.scopeName = NULL;
#endif
		r = makeTagEntry (&x);
	}
	return r;
}

extern void initTagEntry (tagEntryInfo *const e, const char *const name,
			  const kindOption *kind)
{
	initTagEntryFull(e, name,
			 getInputLineNumber (),
			 getInputLanguageName (),
			 getInputFilePosition (),
			 getInputFileTagPath (),
			 kind,
			 ROLE_INDEX_DEFINITION,
			 getSourceFileTagPath(),
			 getSourceLanguageName(),
			 getSourceLineNumber() - getInputLineNumber ());
}

extern void initRefTagEntry (tagEntryInfo *const e, const char *const name,
			     const kindOption *kind, int roleIndex)
{
	initTagEntryFull(e, name,
			 getInputLineNumber (),
			 getInputLanguageName (),
			 getInputFilePosition (),
			 getInputFileTagPath (),
			 kind,
			 roleIndex,
			 getSourceFileTagPath(),
			 getSourceLanguageName(),
			 getSourceLineNumber() - getInputLineNumber ());
}

extern void initTagEntryFull (tagEntryInfo *const e, const char *const name,
			      unsigned long lineNumber,
			      const char* language,
			      MIOPos      filePosition,
			      const char *inputFileName,
			      const kindOption *kind,
			      int roleIndex,
			      const char *sourceFileName,
			      const char* sourceLanguage,
			      long sourceLineNumberDifference)
{
	Assert (File.input.name != NULL);

	memset (e, 0, sizeof (tagEntryInfo));
	e->lineNumberEntry = (boolean) (Option.locate == EX_LINENUM);
	e->lineNumber      = lineNumber;
	e->language        = language;
	e->filePosition    = filePosition;
	e->inputFileName   = inputFileName;
	e->name            = name;
	e->extensionFields.scopeIndex     = SCOPE_NIL;
	e->kind = kind;

	Assert (roleIndex >= ROLE_INDEX_DEFINITION);
	Assert (kind == NULL || roleIndex < kind->nRoles);
	e->extensionFields.roleIndex = roleIndex;
	if (roleIndex > ROLE_INDEX_DEFINITION)
		markTagExtraBit (e, XTAG_REFERENCE_TAGS);

	e->sourceLanguage = sourceLanguage;
	e->sourceFileName = sourceFileName;
	e->sourceLineNumberDifference = sourceLineNumberDifference;
}

extern void    markTagExtraBit     (tagEntryInfo *const tag, xtagType extra)
{
	unsigned int index;
	unsigned int offset;

	Assert (extra < XTAG_COUNT);
	Assert (extra != XTAG_UNKNOWN);

	index = (extra / 8);
	offset = (extra % 8);
	tag->extra [ index ] |= (1 << offset);
}

extern boolean isTagExtraBitMarked (const tagEntryInfo *const tag, xtagType extra)
{
	unsigned int index = (extra / 8);
	unsigned int offset = (extra % 8);

	Assert (extra < XTAG_COUNT);
	Assert (extra != XTAG_UNKNOWN);

	return !! ((tag->extra [ index ]) & (1 << offset));
}

extern unsigned long numTagsAdded(void)
{
	return TagFile.numTags.added;
}

extern void setNumTagsAdded (unsigned long nadded)
{
	TagFile.numTags.added = nadded;
}

extern unsigned long numTagsTotal(void)
{
	return TagFile.numTags.added + TagFile.numTags.prev;
}

extern unsigned long maxTagsLine (void)
{
	return (unsigned long)TagFile.max.line;
}

extern void invalidatePatternCache(void)
{
	TagFile.patternCacheValid = FALSE;
}

extern void tagFilePosition (MIOPos *p)
{
	mio_getpos (TagFile.fp, p);
}

extern void setTagFilePosition (MIOPos *p)
{
	mio_setpos (TagFile.fp, p);
}

extern const char* getTagFileDirectory (void)
{
	return TagFile.directory;
}

/* vi:set tabstop=4 shiftwidth=4: */
