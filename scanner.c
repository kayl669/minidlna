/* MiniDLNA media server
 * Copyright (C) 2008-2009  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <locale.h>
#include <libgen.h>
#include <inttypes.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "config.h"

#ifdef ENABLE_NLS
#include <libintl.h>
#endif
#include "libav.h"

#include "scanner_sqlite.h"
#include "upnpglobalvars.h"
#include "metadata.h"
#include "playlist.h"
#include "utils.h"
#include "sql.h"
#include "scanner.h"
#include "albumart.h"
#include "containers.h"
#include "log.h"

#if SCANDIR_CONST
typedef const struct dirent scan_filter;
#else
typedef struct dirent scan_filter;
#endif
#ifndef AV_LOG_PANIC
#define AV_LOG_PANIC AV_LOG_FATAL
#endif

int valid_cache = 0;

struct virtual_item
{
	sqlite_int64 objectID;
	char parentID[64];
	char name[256];
};

sqlite_int64
get_next_available_id(const char *table, const char *parentID)
{
		char *ret, *base;
		sqlite_int64 objectID = 0;

		ret = sql_get_text_field(db, "SELECT OBJECT_ID from %s where ID = "
		                             "(SELECT max(ID) from %s where PARENT_ID = %Q)",
		                             table, table, parentID);
		if( ret )
		{
			base = strrchr(ret, '$');
			if( base )
				objectID = strtoll(base+1, NULL, 16) + 1;
			sqlite3_free(ret);
		}

		return objectID;
}

int
insert_container(const char *item, const char *rootParent, const char *refID, const char *class,
                 const char *artist, const char *genre, const char *album_art, sqlite_int64 *objectID, sqlite_int64 *parentID)
{
	char *result;
	char *base;
	int ret = 0;

	result = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS o "
	                                "left join DETAILS d on (o.DETAIL_ID = d.ID)"
	                                " where o.PARENT_ID = '%s'"
			                " and o.NAME like %Q"
			                " and d.ARTIST %s %Q"
	                                " and o.CLASS = 'container.%s' limit 1",
	                                rootParent, item, artist?"like":"is", artist, class);
	if( result )
	{
		base = strrchr(result, '$');
		if( base )
			*parentID = strtoll(base+1, NULL, 16);
		else
			*parentID = 0;
		*objectID = get_next_available_id("OBJECTS", result);
	}
	else
	{
		sqlite_int64 detailID = 0;
		*objectID = 0;
		*parentID = get_next_available_id("OBJECTS", rootParent);
		if( refID )
		{
			result = sql_get_text_field(db, "SELECT DETAIL_ID from OBJECTS where OBJECT_ID = %Q", refID);
			if( result )
				detailID = strtoll(result, NULL, 10);
		}
		if( !detailID )
		{
			detailID = GetFolderMetadata(item, NULL, artist, genre, (album_art ? strtoll(album_art, NULL, 10) : 0));
		}
		ret = sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, DETAIL_ID, CLASS, NAME) "
						   "VALUES ('%q$%"PRIX64"', %Q, %Q, %"PRId64", 'container.%q', %Q)",
		                   rootParent, *parentID, rootParent, refID, detailID, class, item);
	}
	sqlite3_free(result);

	return ret;
}

static void
insert_containers(const char *name, const char *path, const char *refID, const char *class, sqlite_int64 detailID)
{
	char *sql;
	char **result;
	int ret;
	int cols, row;
	sqlite_int64 objectID, parentID;

	if( strstr(class, "imageItem") )
	{
		char *date_taken = NULL, *camera = NULL;
		static struct virtual_item last_date;
		static struct virtual_item last_cam;
		static struct virtual_item last_camdate;

		sql = sqlite3_mprintf("SELECT DATE, CREATOR from DETAILS where ID = %"PRId64, detailID);
		ret = sql_get_table(db, sql, &result, &row, &cols);
		sqlite3_free(sql);
		if( ret == SQLITE_OK )
		{
			date_taken = result[2];
			camera = result[3];
		}
		if( date_taken )
			date_taken[10] = '\0';
		else
			date_taken = _("Date Inconnue");
		if( !camera )
			camera = _("Caméra Inconnue");

		if( valid_cache && strcmp(last_date.name, date_taken) == 0 )
		{
			last_date.objectID++;
			//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Using last date item: %s/%s/%X\n", last_date.name, last_date.parentID, last_date.objectID);
		}
		else
		{
			insert_container(date_taken, IMAGE_DATE_ID, NULL, "album.photoAlbum", NULL, NULL, NULL, &objectID, &parentID);
			sprintf(last_date.parentID, IMAGE_DATE_ID"$%"PRIX64, parentID);
			last_date.objectID = objectID;
			strncpyt(last_date.name, date_taken, sizeof(last_date.name));
			//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Creating cached date item: %s/%s/%X\n", last_date.name, last_date.parentID, last_date.objectID);
		}
		sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
					 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
		             last_date.parentID, last_date.objectID, last_date.parentID, refID, class, detailID, name);

		if( !valid_cache || strcmp(camera, last_cam.name) != 0 )
		{
			insert_container(camera, IMAGE_CAMERA_ID, NULL, "storageFolder", NULL, NULL, NULL, &objectID, &parentID);
			sprintf(last_cam.parentID, IMAGE_CAMERA_ID"$%"PRIX64, parentID);
			strncpyt(last_cam.name, camera, sizeof(last_cam.name));
			/* Invalidate last_camdate cache */
			last_camdate.name[0] = '\0';
		}
		if( valid_cache && strcmp(last_camdate.name, date_taken) == 0 )
		{
			last_camdate.objectID++;
			//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Using last camdate item: %s/%s/%s/%X\n", camera, last_camdate.name, last_camdate.parentID, last_camdate.objectID);
		}
		else
		{
			insert_container(date_taken, last_cam.parentID, NULL, "album.photoAlbum", NULL, NULL, NULL, &objectID, &parentID);
			sprintf(last_camdate.parentID, "%s$%"PRIX64, last_cam.parentID, parentID);
			last_camdate.objectID = objectID;
			strncpyt(last_camdate.name, date_taken, sizeof(last_camdate.name));
			//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Creating cached camdate item: %s/%s/%s/%X\n", camera, last_camdate.name, last_camdate.parentID, last_camdate.objectID);
		}
		sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
					 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
		             last_camdate.parentID, last_camdate.objectID, last_camdate.parentID, refID, class, detailID, name);
		/* All Images */
		sqlite_int64 last_all_objectID = get_next_available_id("OBJECTS", IMAGE_ALL_ID);
		sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
					 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
		             IMAGE_ALL_ID, last_all_objectID, IMAGE_ALL_ID, refID, class, detailID, name);
	}
	else if( strstr(class, "audioItem") )
	{
		sql = sqlite3_mprintf("SELECT ALBUM, ARTIST, GENRE, ALBUM_ART from DETAILS where ID = %"PRId64, detailID);
		ret = sql_get_table(db, sql, &result, &row, &cols);
		sqlite3_free(sql);
		if( ret != SQLITE_OK )
			return;
		if( !row )
		{
			sqlite3_free_table(result);
			return;
		}
		char *album = result[4], *artist = result[5], *genre = result[6];
		char *album_art = result[7];
		static struct virtual_item last_album;
		static struct virtual_item last_artist;
		static struct virtual_item last_artistAlbum;
		static struct virtual_item last_artistAlbumAll;
		static struct virtual_item last_genre;
		static struct virtual_item last_genreArtist;
		static struct virtual_item last_genreArtistAll;

		if( album )
		{
			if( valid_cache && strcmp(album, last_album.name) == 0 )
			{
				last_album.objectID++;
				//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Using last album item: %s/%s/%X\n", last_album.name, last_album.parentID, last_album.objectID);
			}
			else
			{
				strncpyt(last_album.name, album, sizeof(last_album.name));
				insert_container(album, MUSIC_ALBUM_ID, NULL, "album.musicAlbum", artist, genre, album_art, &objectID, &parentID);
				sprintf(last_album.parentID, MUSIC_ALBUM_ID"$%"PRIX64, parentID);
				last_album.objectID = objectID;
				//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Creating cached album item: %s/%s/%X\n", last_album.name, last_album.parentID, last_album.objectID);
			}
			sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
						 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
			             last_album.parentID, last_album.objectID, last_album.parentID, refID, class, detailID, name);
		}
		if( artist )
		{
			if( !valid_cache || strcmp(artist, last_artist.name) != 0 )
			{
				insert_container(artist, MUSIC_ARTIST_ID, NULL, "person.musicArtist", NULL, genre, NULL, &objectID, &parentID);
				sprintf(last_artist.parentID, MUSIC_ARTIST_ID"$%"PRIX64, parentID);
				strncpyt(last_artist.name, artist, sizeof(last_artist.name));
				last_artistAlbum.name[0] = '\0';
				/* Add this file to the "- All Albums -" container as well */
				insert_container(_("- Tous les Albums -"), last_artist.parentID, NULL, "album", artist, genre, NULL, &objectID, &parentID);
				sprintf(last_artistAlbumAll.parentID, "%s$%"PRIX64, last_artist.parentID, parentID);
				last_artistAlbumAll.objectID = objectID;
			}
			else
			{
				last_artistAlbumAll.objectID++;
			}
			if( valid_cache && strcmp(album?album:_("Album inconnu"), last_artistAlbum.name) == 0 )
			{
				last_artistAlbum.objectID++;
				//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Using last artist/album item: %s/%s/%X\n", last_artist.name, last_artist.parentID, last_artist.objectID);
			}
			else
			{
				insert_container(album?album:_("Album inconnu"), last_artist.parentID, album?last_album.parentID:NULL,
				                 "album.musicAlbum", artist, genre, album_art, &objectID, &parentID);
				sprintf(last_artistAlbum.parentID, "%s$%"PRIX64, last_artist.parentID, parentID);
				last_artistAlbum.objectID = objectID;
				strncpyt(last_artistAlbum.name, album ? album : _("Album inconnu"), sizeof(last_artistAlbum.name));
				//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Creating cached artist/album item: %s/%s/%X\n", last_artist.name, last_artist.parentID, last_artist.objectID);
			}
			sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
						 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
			             last_artistAlbum.parentID, last_artistAlbum.objectID, last_artistAlbum.parentID, refID, class, detailID, name);
			sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
						 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
			             last_artistAlbumAll.parentID, last_artistAlbumAll.objectID, last_artistAlbumAll.parentID, refID, class, detailID, name);
		}
		if( genre )
		{
			if( !valid_cache || strcmp(genre, last_genre.name) != 0 )
			{
				insert_container(genre, MUSIC_GENRE_ID, NULL, "genre.musicGenre", NULL, NULL, NULL, &objectID, &parentID);
				sprintf(last_genre.parentID, MUSIC_GENRE_ID"$%"PRIX64, parentID);
				strncpyt(last_genre.name, genre, sizeof(last_genre.name));
				/* Add this file to the "- All Artists -" container as well */
				insert_container(_("- Tous les Artistes -"), last_genre.parentID, NULL, "person", NULL, genre, NULL, &objectID, &parentID);
				sprintf(last_genreArtistAll.parentID, "%s$%"PRIX64, last_genre.parentID, parentID);
				last_genreArtistAll.objectID = objectID;
			}
			else
			{
				last_genreArtistAll.objectID++;
			}
			if( valid_cache && strcmp(artist?artist:_("Artiste inconnu"), last_genreArtist.name) == 0 )
			{
				last_genreArtist.objectID++;
			}
			else
			{
				insert_container(artist?artist:_("Artiste inconnu"), last_genre.parentID, artist?last_artist.parentID:NULL,
				                 "person.musicArtist", NULL, genre, NULL, &objectID, &parentID);
				sprintf(last_genreArtist.parentID, "%s$%"PRIX64, last_genre.parentID, parentID);
				last_genreArtist.objectID = objectID;
				strncpyt(last_genreArtist.name, artist ? artist : _("Artiste inconnu"), sizeof(last_genreArtist.name));
				//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Creating cached genre/artist item: %s/%s/%X\n", last_genreArtist.name, last_genreArtist.parentID, last_genreArtist.objectID);
			}
			sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
						 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
			             last_genreArtist.parentID, last_genreArtist.objectID, last_genreArtist.parentID, refID, class, detailID, name);
			sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
						 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
			             last_genreArtistAll.parentID, last_genreArtistAll.objectID, last_genreArtistAll.parentID, refID, class, detailID, name);
		}
		/* All Music */
		sqlite_int64 last_all_objectID = get_next_available_id("OBJECTS", MUSIC_ALL_ID);
		sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
					 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
					 MUSIC_ALL_ID, last_all_objectID, MUSIC_ALL_ID, refID, class, detailID, name);
	}
	else if( strstr(class, "videoItem") )
	{
		sqlite_int64 last_all_objectID = get_next_available_id("OBJECTS", VIDEO_ALL_ID);
		sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
					 "VALUES ('%q$%"PRIX64"', %Q, %Q, %Q, %"PRId64", %Q)",
					 VIDEO_ALL_ID, last_all_objectID, VIDEO_ALL_ID, refID, class, detailID, name);
		return;
	}
	else
	{
		return;
	}
	sqlite3_free_table(result);
	valid_cache = 1;
}

sqlite_int64
insert_directory(const char *name, const char *path, const char *base, const char *parentID, sqlite_int64 objectID)
{
	sqlite_int64 detailID = 0;
	char class[] = "container.storageFolder";
	char refID[64];
	char *p;
	static char last_found[256] = "-1";

    snprintf(refID, sizeof(refID), "%s%s$%"PRIX64, BROWSEDIR_ID, parentID, objectID);
	if( strcmp(base, BROWSEDIR_ID) != 0 )
	{
		int found = 0;
		char id_buf[64], parent_buf[64];
		char *dir_buf, *dir;

 		dir_buf = strdup(path);
		dir = dirname(dir_buf);
		snprintf(id_buf, sizeof(id_buf), "%s%s$%"PRIX64, base, parentID, objectID);
		snprintf(parent_buf, sizeof(parent_buf), "%s%s", base, parentID);
		while( !found )
		{
			if( valid_cache && strcmp(id_buf, last_found) == 0 )
				break;
			if( sql_get_int_field(db, "SELECT count(*) from OBJECTS where OBJECT_ID = %Q", id_buf) > 0 )
			{
				strcpy(last_found, id_buf);
				break;
			}
			/* Does not exist.  Need to create, and may need to create parents also */
			detailID = sql_get_int64_field(db, "SELECT DETAIL_ID from OBJECTS where OBJECT_ID = %Q", refID);
			sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, DETAIL_ID, CLASS, NAME) "
							 "VALUES (%Q, %Q, %Q, %"PRId64", '%s', %Q)",
			             id_buf, parent_buf, refID, detailID, class, strrchr(dir, '/')+1);
			if( (p = strrchr(id_buf, '$')) )
				*p = '\0';
			if( (p = strrchr(parent_buf, '$')) )
				*p = '\0';
			if( (p = strrchr(refID, '$')) )
				*p = '\0';
			dir = dirname(dir);
		}
		free(dir_buf);
		return 0;
	}

	detailID = GetFolderMetadata(name, path, NULL, NULL, find_album_art(path, NULL, 0));
	sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, DETAIL_ID, CLASS, NAME) "
				 "VALUES ('%s%s$%"PRIX64"', '%q%q', %Q, %"PRId64", '%s', %Q)",
	             base, parentID, objectID, base, parentID, refID, detailID, class, name);

	return detailID;
}

int
insert_file(char *name, const char *path, const char *parentID, media_types types)
{
	char class[32];
	char objectID[64];
	char parent_ID[64];
	sqlite_int64 detailID = 0;
	char base[8];
	char *typedir_parentID;
	char *baseid;
	char *orig_name = strdup(name);

	if( (types & TYPE_IMAGES) && is_image(name) )
	{
		if( is_album_art(name) )
			return -1;
		strcpy(base, IMAGE_DIR_ID);
		strcpy(class, "item.imageItem.photo");
		detailID = GetImageMetadata(path, name);
	}
	else if( (types & TYPE_VIDEO) && is_video(name) )
	{
		strcpy(base, VIDEO_DIR_ID);
		strcpy(class, "item.videoItem");
		detailID = GetVideoMetadata(path, name);
	}
	else if( is_playlist(name) )
	{
		if( insert_playlist(path, name) == 0 )
			return 1;
	}
	if( !detailID && (types & TYPE_AUDIO) && is_audio(name) )
	{
		strcpy(base, MUSIC_DIR_ID);
		strcpy(class, "item.audioItem.musicTrack");
		detailID = GetAudioMetadata(path, name);
	}
	strcpy(name, orig_name);
	if( !detailID )
	{
		DPRINTF(E_WARN, L_SCANNER, "Unsuccessful getting details for %s!\n", path);
		return -1;
	}

	sprintf(parent_ID, "%s%s", BROWSEDIR_ID, parentID);
	sqlite_int64 object = get_next_available_id("OBJECTS", parent_ID);
	sprintf(objectID, "%s$%"PRIX64, parent_ID, object);

	sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, CLASS, DETAIL_ID, NAME) "
					 "VALUES (%Q, %Q, '%s', %"PRId64", %Q)",
					 objectID, parent_ID, class, detailID, name);

	if( *parentID )
	{
		int typedir_objectID = 0;
		typedir_parentID = strdup(parentID);
		baseid = strrchr(typedir_parentID, '$');
		if( baseid )
		{
			typedir_objectID = strtol(baseid+1, NULL, 16);
			*baseid = '\0';
		}
		insert_directory(name, path, base, typedir_parentID, typedir_objectID);
		free(typedir_parentID);
	}
	strcpy(name, orig_name);
	sprintf(parent_ID, "%s%s", base, parentID);
	sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, REF_ID, CLASS, DETAIL_ID, NAME) "
	             "VALUES ('%s$%"PRIX64"', %Q, %Q, '%s', %"PRId64", %Q)",
	             parent_ID, object, parent_ID, objectID, class, detailID, name);

	insert_containers(name, path, objectID, class, detailID);

	if( is_video(orig_name) )
	{
		DIR * ds;
		struct dirent *e;

		char *nPath = strdup(path);
		char *nFile = strdup(path);
		strip_ext(nFile);
		dirname(nPath);
		ds = opendir(nPath);
		if( ds != NULL )
		{
			while( (e = readdir(ds)) )
			{
					char *cmpFile = malloc(PATH_MAX);
					sprintf(cmpFile, "%s/%s", nPath, e->d_name);
				if( access(cmpFile, R_OK) == 0)
				{
					strip_ext(cmpFile);
					strip_ext(cmpFile);
					if(strcmp(nFile, cmpFile) == 0 && ends_with(e->d_name, ".srt"))
					{
						sprintf(cmpFile, "%s/%s", nPath, e->d_name);
						check_for_captions(cmpFile);
					}
				}
					free(cmpFile);
				}
			}
		closedir(ds);
		free(nFile);
		free(nPath);
	}

	free(orig_name);

	return 0;
}

int
CreateDatabase(void)
{
	int ret, i;
	const char *containers[] = { "0","-1",   "root",
	                        MUSIC_ID, "0", _("Musique"),
	                    MUSIC_ALL_ID, MUSIC_ID, _("Toute la musique"),
	                  MUSIC_GENRE_ID, MUSIC_ID, _("Genre"),
	                 MUSIC_ARTIST_ID, MUSIC_ID, _("Artiste"),
	                  MUSIC_ALBUM_ID, MUSIC_ID, _("Album"),
	                    MUSIC_DIR_ID, MUSIC_ID, _("Dossiers"),
	                  MUSIC_PLIST_ID, MUSIC_ID, _("Liste de Lecture"),

	                        VIDEO_ID, "0", _("Vidéo"),
	                    VIDEO_ALL_ID, VIDEO_ID, _("Toutes les vidéos"),
	                    VIDEO_DIR_ID, VIDEO_ID, _("Dossiers"),

	                        IMAGE_ID, "0", _("Images"),
	                    IMAGE_ALL_ID, IMAGE_ID, _("Toutes les images"),
	                   IMAGE_DATE_ID, IMAGE_ID, _("Date de prise"),
	                 IMAGE_CAMERA_ID, IMAGE_ID, _("Caméra"),
	                    IMAGE_DIR_ID, IMAGE_ID, _("Dossiers"),

	                    BROWSEDIR_ID, "0", _("Parcourir les Dossiers"),
			0 };

	ret = sql_exec(db, create_objectTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, create_detailTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, create_albumArtTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, create_captionTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, create_bookmarkTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, create_playlistTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, create_settingsTable_sqlite);
	if( ret != SQLITE_OK )
		goto sql_failed;
	ret = sql_exec(db, "INSERT into SETTINGS values ('UPDATE_ID', '0')");
	if( ret != SQLITE_OK )
		goto sql_failed;
	for( i=0; containers[i]; i=i+3 )
	{
		ret = sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, DETAIL_ID, CLASS, NAME)"
		                   " VALUES (%Q, %Q, %lld, 'container.storageFolder', %Q)",
		                   containers[i], containers[i+1], GetFolderMetadata(containers[i+2], NULL, NULL, NULL, 0), containers[i+2]);
		if( ret != SQLITE_OK )
			goto sql_failed;
	}
	for( i=0; magic_containers[i].objectid_match; i++ )
	{
		struct magic_container_s *magic = &magic_containers[i];
		if (!magic->name)
			continue;
		if( sql_get_int_field(db, "SELECT 1 from OBJECTS where OBJECT_ID = '%s'", magic->objectid_match) == 0 )
		{
			char *parent = strdup(magic->objectid_match);
			if (strrchr(parent, '$'))
				*strrchr(parent, '$') = '\0';
			ret = sql_exec(db, "INSERT into OBJECTS (OBJECT_ID, PARENT_ID, DETAIL_ID, CLASS, NAME)"
			                   " VALUES (%Q, %Q, %"PRId64", 'container.storageFolder', %Q)",
					   magic->objectid_match, parent, GetFolderMetadata(magic->name, NULL, NULL, NULL, 0), magic->name);
			free(parent);
			if( ret != SQLITE_OK )
				goto sql_failed;
		}
	}
	sql_exec(db, "create INDEX IDX_OBJECTS_OBJECT_ID ON OBJECTS(OBJECT_ID);");
	sql_exec(db, "create INDEX IDX_OBJECTS_PARENT_ID ON OBJECTS(PARENT_ID);");
	sql_exec(db, "create INDEX IDX_OBJECTS_DETAIL_ID ON OBJECTS(DETAIL_ID);");
	sql_exec(db, "create INDEX IDX_OBJECTS_CLASS ON OBJECTS(CLASS);");
	sql_exec(db, "create INDEX IDX_DETAILS_PATH ON DETAILS(PATH);");
	sql_exec(db, "create INDEX IDX_DETAILS_ID ON DETAILS(ID);");
	sql_exec(db, "create INDEX IDX_ALBUM_ART ON ALBUM_ART(ID);");
	sql_exec(db, "create INDEX IDX_SCANNER_OPT ON OBJECTS(PARENT_ID, NAME, OBJECT_ID);");

sql_failed:
	if( ret != SQLITE_OK )
		DPRINTF(E_ERROR, L_DB_SQL, "Error creating SQLite3 database!\n");
	return (ret != SQLITE_OK);
}

static inline int
filter_hidden(scan_filter *d)
{
	return (d->d_name[0] != '.');
}

static int
filter_type(scan_filter *d)
{
#if HAVE_STRUCT_DIRENT_D_TYPE
	return ( (d->d_type == DT_DIR) ||
	         (d->d_type == DT_LNK) ||
	         (d->d_type == DT_UNKNOWN)
	       );
#else
	return 1;
#endif
}

static int
filter_a(scan_filter *d)
{
	return ( filter_hidden(d) &&
	         (filter_type(d) ||
		  (is_reg(d) &&
		   (is_audio(d->d_name) ||
	            is_playlist(d->d_name))))
	       );
}

static int
filter_av(scan_filter *d)
{
	return ( filter_hidden(d) &&
	         (filter_type(d) ||
		  (is_reg(d) &&
		   (is_audio(d->d_name) ||
		    is_video(d->d_name) ||
	            is_playlist(d->d_name))))
	       );
}

static int
filter_ap(scan_filter *d)
{
	return ( filter_hidden(d) &&
	         (filter_type(d) ||
		  (is_reg(d) &&
		   (is_audio(d->d_name) ||
		    is_image(d->d_name) ||
	            is_playlist(d->d_name))))
	       );
}

static int
filter_v(scan_filter *d)
{
	return ( filter_hidden(d) &&
	         (filter_type(d) ||
		  (is_reg(d) &&
	           is_video(d->d_name)))
	       );
}

static int
filter_vp(scan_filter *d)
{
	return ( filter_hidden(d) &&
	         (filter_type(d) ||
		  (is_reg(d) &&
		   (is_video(d->d_name) ||
	            is_image(d->d_name))))
	       );
}

static int
filter_p(scan_filter *d)
{
	return ( filter_hidden(d) &&
	         (filter_type(d) ||
		  (is_reg(d) &&
		   is_image(d->d_name)))
	       );
}

static int
filter_avp(scan_filter *d)
{
	return ( filter_hidden(d) &&
	         (filter_type(d) ||
		  (is_reg(d) &&
		   (is_audio(d->d_name) ||
		    is_image(d->d_name) ||
		    is_video(d->d_name) ||
	            is_playlist(d->d_name))))
	       );
}

static void
ScanDirectory(const char *dir, const char *parent, media_types dir_types)
{
	struct dirent **namelist;
	int i, n;
	char *full_path;
	char *name = NULL;
	static long long unsigned int fileno = 0;
	enum file_types type;

	DPRINTF(parent?E_INFO:E_WARN, L_SCANNER, _("Scanning %s\n"), dir);
	switch( dir_types )
	{
		case ALL_MEDIA:
			n = scandir(dir, &namelist, filter_avp, alphasort);
			break;
		case TYPE_AUDIO:
			n = scandir(dir, &namelist, filter_a, alphasort);
			break;
		case TYPE_AUDIO|TYPE_VIDEO:
			n = scandir(dir, &namelist, filter_av, alphasort);
			break;
		case TYPE_AUDIO|TYPE_IMAGES:
			n = scandir(dir, &namelist, filter_ap, alphasort);
			break;
		case TYPE_VIDEO:
			n = scandir(dir, &namelist, filter_v, alphasort);
			break;
		case TYPE_VIDEO|TYPE_IMAGES:
			n = scandir(dir, &namelist, filter_vp, alphasort);
			break;
		case TYPE_IMAGES:
			n = scandir(dir, &namelist, filter_p, alphasort);
			break;
		default:
			n = -1;
			break;
	}
	if( n < 0 )
	{
		DPRINTF(E_WARN, L_SCANNER, "Error scanning %s\n", dir);
		return;
	}

	full_path = malloc(PATH_MAX);
	if (!full_path)
	{
		DPRINTF(E_ERROR, L_SCANNER, "Memory allocation failed scanning %s\n", dir);
		return;
	}

	for (i=0; i < n; i++)
	{
#if !USE_FORK
		if( quitting )
			break;
#endif
		type = TYPE_UNKNOWN;
		snprintf(full_path, PATH_MAX, "%s/%s", dir, namelist[i]->d_name);
		name = escape_tag(namelist[i]->d_name, 1);
		if( is_dir(namelist[i]) == 1 )
		{
			type = TYPE_DIR;
		}
		else if( is_reg(namelist[i]) == 1 )
		{
			type = TYPE_FILE;
		}
		else
		{
			type = resolve_unknown_type(full_path, dir_types);
		}
		if( (type == TYPE_DIR) && (access(full_path, R_OK|X_OK) == 0) )
		{
			char *parent_id;
			xasprintf(&parent_id, "%s%s", BROWSEDIR_ID, (parent ? parent:""));
			sqlite_int64 parentId = get_next_available_id("OBJECTS", parent_id);
			insert_directory(name, full_path, BROWSEDIR_ID, THISORNUL(parent), parentId);
			xasprintf(&parent_id, "%s$%"PRIX64, THISORNUL(parent), parentId);
			ScanDirectory(full_path, parent_id, dir_types);
			free(parent_id);
		}
		else if( type == TYPE_FILE && (access(full_path, R_OK) == 0) )
		{
			if( insert_file(name, full_path, THISORNUL(parent), dir_types) == 0 )
				fileno++;
		}
		free(name);
		free(namelist[i]);
	}
	free(namelist);
	free(full_path);
	if( !parent )
	{
		DPRINTF(E_WARN, L_SCANNER, _("Scanning %s finished (%llu files)!\n"), dir, fileno);
	}
}

static void
_notify_start(void)
{
#ifdef READYNAS
	FILE *flag = fopen("/ramfs/.upnp-av_scan", "w");
	if( flag )
		fclose(flag);
#endif
}

static void
_notify_stop(void)
{
#ifdef READYNAS
	if( access("/ramfs/.rescan_done", F_OK) == 0 )
		system("/bin/sh /ramfs/.rescan_done");
	unlink("/ramfs/.upnp-av_scan");
#endif
}

void
start_scanner()
{
	struct media_dir_s *media_path;
	char path[MAXPATHLEN];

	if (setpriority(PRIO_PROCESS, 0, 15) == -1)
		DPRINTF(E_WARN, L_INOTIFY,  "Failed to reduce scanner thread priority\n");
	_notify_start();

	setlocale(LC_COLLATE, "");

	av_register_all();
	av_log_set_level(AV_LOG_PANIC);
	for( media_path = media_dirs; media_path != NULL; media_path = media_path->next )
	{
		sqlite_int64 id;
		char *bname, *parent = NULL;
		char buf[8];
		strncpyt(path, media_path->path, sizeof(path));
		bname = basename(path);
		/* If there are multiple media locations, add a level to the ContentDirectory */
		if( !GETFLAG(MERGE_MEDIA_DIRS_MASK) && media_dirs->next )
		{
			sqlite_int64 startID = get_next_available_id("OBJECTS", BROWSEDIR_ID);
			id = insert_directory(bname, path, BROWSEDIR_ID, "", startID);
			sprintf(buf, "$%"PRIX64, startID);
			parent = buf;
		}
		else
			id = GetFolderMetadata(bname, media_path->path, NULL, NULL, 0);
		/* Use TIMESTAMP to store the media type */
		sql_exec(db, "UPDATE DETAILS set TIMESTAMP = %d where ID = %"PRId64, media_path->types, id);
		ScanDirectory(media_path->path, parent, media_path->types);
		sql_exec(db, "INSERT into SETTINGS values (%Q, %Q)", "media_dir", media_path->path);
	}
	_notify_stop();
	/* Create this index after scanning, so it doesn't slow down the scanning process.
	 * This index is very useful for large libraries used with an XBox360 (or any
	 * client that uses UPnPSearch on large containers). */
	sql_exec(db, "create INDEX IDX_SEARCH_OPT ON OBJECTS(OBJECT_ID, CLASS, DETAIL_ID);");

	if( GETFLAG(NO_PLAYLIST_MASK) )
	{
		DPRINTF(E_WARN, L_SCANNER, "Playlist creation disabled\n");	  
	}
	else
	{
		fill_playlists();
	}

	DPRINTF(E_DEBUG, L_SCANNER, "Initial file scan completed\n");
	//JM: Set up a db version number, so we know if we need to rebuild due to a new structure.
	sql_exec(db, "pragma user_version = %d;", DB_VERSION);
}
