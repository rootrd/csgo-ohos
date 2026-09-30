//========= Copyright (c) 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
// $NoKeywords: $  
//=============================================================================//

#include <dirent.h>
#include <errno.h>

#include "tier1/strtools.h"
#include "tier0/memdbgoff.h"
#include "linux_support.h"

#ifdef OSX
#include <AvailabilityMacros.h>
#endif

char selectBuf[PATH_MAX];

#if defined(OSX) && !defined(MAC_OS_X_VERSION_10_9)
int FileSelect(struct dirent *ent)
#else
int FileSelect(const struct dirent *ent)
#endif
{
	const char *mask=selectBuf;
	const char *name=ent->d_name;
	
	//printf("Test:%s %s\n",mask,name);
	
	if(!strcmp(name,".") || !strcmp(name,"..") ) return 0;
	
	if(!strcmp(selectBuf,"*.*")) return 1;
	
	while( *mask && *name )
	{
		if(*mask=='*')
		{
			mask++; // move to the next char in the mask
			if(!*mask) // if this is the end of the mask its a match 
			{
				return 1;
			}
			while(*name && toupper(*name)!=toupper(*mask)) 
			{ // while the two don't meet up again
				name++;
			}
			if(!*name) 
			{ // end of the name
				break; 
			}
		}
		else if (*mask!='?')
		{
			if( toupper(*mask) != toupper(*name) )
			{	// mismatched!
				return 0;
			}
			else
			{	
				mask++;
				name++;
				if( !*mask && !*name) 
				{ // if its at the end of the buffer
					return 1;
				}
				
			}
			
		}
		else /* mask is "?", we don't care*/
		{
			mask++;
			name++;
		}
	}	
	
	return( !*mask && !*name ); // both of the strings are at the end
}

int FillDataStruct(FIND_DATA *dat)
{
	struct stat fileStat;
	
	if(dat->numMatches<0)
		return -1;
	
	char szFullPath[MAX_PATH];
	Q_snprintf( szFullPath, sizeof(szFullPath), "%s/%s", dat->cBaseDir, dat->namelist[dat->numMatches]->d_name );  
	
	if(!stat(szFullPath,&fileStat))
	{
		dat->dwFileAttributes=fileStat.st_mode;           
	}
	else
	{
		dat->dwFileAttributes=0;
	}	
	
	// now just put the filename in the output data
	Q_snprintf( dat->cFileName, sizeof(dat->cFileName), "%s", dat->namelist[dat->numMatches]->d_name );  
	
	//printf("%s\n", dat->namelist[dat->numMatches]->d_name);
	free(dat->namelist[dat->numMatches]);
	
  	dat->numMatches--;
	return 1;
}


HANDLE FindFirstFile( const char *fileName, FIND_DATA *dat)
{
	char nameStore[PATH_MAX];
	char *dir = nullptr;
	int n,iret = -1;


	bool foundValidDir = false;

	dat->numMatches = -1;
	dat->namelist = nullptr;
	dat->cBaseDir[0] = '\0';
	
	Q_strncpy(nameStore,fileName, sizeof( nameStore ) );
	
	if(strrchr(nameStore,'/') )
	{
		dir=nameStore;
		while(strrchr(dir,'/') )
		{
			struct stat dirChk;
			
			// zero this with the dir name
			dir=strrchr(nameStore,'/');
			*dir='\0';
			
			dir=nameStore;
			stat(dir,&dirChk);
			
			if( S_ISDIR( dirChk.st_mode ) )
			{
				foundValidDir = true;
				break;	
			}
		}
	}
	
	if( dir != nullptr && foundValidDir && strlen(dir)>0 )
	{
		Q_strncpy(selectBuf,fileName+strlen(dir)+1, sizeof( selectBuf ) );
		Q_strncpy(dat->cBaseDir,dir, sizeof( dat->cBaseDir ) );
		dat->namelist = NULL;
		n = scandir(dir, &dat->namelist, FileSelect, alphasort);
		if (n < 0)
		{
			// silently return, nothing interesting
			dat->namelist = NULL;
		}
		else 
		{
			dat->numMatches=n-1; // n is the number of matches
			iret=FillDataStruct(dat);
			if ( ( iret<0 ) && dat->namelist )
			{
				free(dat->namelist);
				dat->namelist = NULL;
			}
			
		}
	}
	
	//	printf("Returning: %i \n",iret);
	return (HANDLE)(intp)iret;
}

bool FindNextFile(HANDLE handle, FIND_DATA *dat)
{
	if ( -1 == (intp)dat )
	{
		return false;
	}

	if(dat->numMatches<0)
	{	
		if ( dat->namelist != NULL )
		{
			free( dat->namelist );
			dat->namelist = NULL;
		}
		return false; // no matches left
	}	
	
	FillDataStruct(dat);
	return true;
}

bool FindClose(HANDLE handle)
{
	return true;
}



// Resolve each component without shared scratch state: file loading and async
// I/O can ask for differently cased names concurrently. Exact spelling wins.
const char *findFileInDirCaseInsensitive(const char *file, char *pFileNameOut)
{
	if ( !file || !*file || strlen( file ) >= MAX_PATH )
	{
		errno = file && *file ? ENAMETOOLONG : ENOENT;
		return NULL;
	}
	char normalized[MAX_PATH], resolved[MAX_PATH] = {};
	Q_strncpy( normalized, file, sizeof(normalized) );
	Q_FixSlashes( normalized );
	size_t cursor = 0, used = 0;
	if ( normalized[0] == '/' )
	{
		resolved[used++] = '/';
		cursor = 1;
	}
	while ( normalized[cursor] )
	{
		while ( normalized[cursor] == '/' ) ++cursor;
		if ( !normalized[cursor] ) break;
		const size_t begin = cursor;
		while ( normalized[cursor] && normalized[cursor] != '/' ) ++cursor;
		const size_t length = cursor - begin;
		char component[MAX_PATH];
		memcpy( component, normalized + begin, length );
		component[length] = 0;
		if ( used && resolved[used-1] != '/' ) resolved[used++] = '/';
		if ( used + length >= sizeof(resolved) )
		{
			errno = ENAMETOOLONG;
			return NULL;
		}
		memcpy( resolved + used, component, length + 1 );
		struct stat status;
		if ( stat( resolved, &status ) != 0 )
		{
			resolved[used] = 0;
			DIR *directory = opendir( used ? resolved : "." );
			if ( !directory ) return NULL;
			char match[MAX_PATH] = {};
			while ( dirent *entry = readdir( directory ) )
			{
				if ( !strcasecmp( entry->d_name, component ) &&
					 ( !match[0] || strcmp( entry->d_name, match ) < 0 ) )
					Q_strncpy( match, entry->d_name, sizeof(match) );
			}
			closedir( directory );
			if ( !match[0] || used + strlen(match) >= sizeof(resolved) )
			{
				errno = match[0] ? ENAMETOOLONG : ENOENT;
				return NULL;
			}
			Q_strncpy( resolved + used, match, sizeof(resolved) - used );
		}
		used = strlen( resolved );
	}
	Q_strncpy( pFileNameOut, resolved, MAX_PATH );
	return pFileNameOut;
}
