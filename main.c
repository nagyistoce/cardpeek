/********************************************************************** 
*
* This file is part of Cardpeek, the smartcard reader utility.
*
* Copyright 2009-2013 by 'L1L1'
*
* Cardpeek is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* Cardpeek is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with Cardpeek.  If not, see <http://www.gnu.org/licenses/>.
*
*/

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <glib/gstdio.h>
#include "smartcard.h"
#include "misc.h"
#include "a_string.h"
#include "gui.h"
#include "gui_readerview.h"
#include "pathconfig.h"
#include "lua_ext.h"
#include "script_version.h"
#include "system_info.h"
#include <errno.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <signal.h> 
#include <getopt.h>
#include "cardpeek_resources.gresource"

#include "gui_inprogress.h"
#include <curl/curl.h>

static int progress_update_smartcard_list_txt(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    GtkWidget *progress = (GtkWidget *)clientp;

    if (dltotal==0) 
        return !gui_inprogress_pulse(progress);
    return !gui_inprogress_set_fraction(progress,dlnow/dltotal);
}

static int update_smartcard_list_txt(void)
{
    CURL *curl;
    CURLcode res;
    const char* smartcard_list_txt = path_config_get_string(PATH_CONFIG_FILE_SMARTCARD_LIST_TXT);
    const char* smartcard_list_download = path_config_get_string(PATH_CONFIG_FILE_SMARTCARD_LIST_DOWNLOAD);
    FILE* smartcard_list;
    char *url;
    int retval = 0;    
    char user_agent[100];
    time_t now = time(NULL);
    unsigned next_update = (unsigned)luax_variable_get_integer("cardpeek.smartcard_list.next_update");
    GtkWidget *progress;

    if (luax_variable_get_boolean("cardpeek.smartcard_list.auto_update")!=TRUE)
    {
        log_printf(LOG_INFO,"smartcard_list.txt auto-update is disabled.");
        return 0;
    }

    if (now<next_update) return 0;

    switch (gui_question("The local copy of the ATR database may be outdated.\nDo you whish to do an online update?",
                         "Yes","No, ask me again later","No, always use the local copy")) 
    {
        case 0:
            break;
        case 1:
            luax_variable_set_integer("cardpeek.smartcard_list.next_update",(int)(now+(24*3600)));
            return 0;
        case 2:
            luax_variable_set_boolean("cardpeek.smartcard_list.auto_update",FALSE);
            return 0;
        default:
            return 0;
    }

    log_printf(LOG_INFO,"Attempting to update smartcard_list.txt");

    url=luax_variable_get_strdup("cardpeek.smartcard_list.url");

    if (url==NULL)
        url = g_strdup("http://ludovic.rousseau.free.fr/softwares/pcsc-tools/smartcard_list.txt");

    progress = gui_inprogress_new("Downloading file","Please wait...");

    curl = curl_easy_init();

    if (curl) 
    {
        g_sprintf(user_agent,"cardpeek %s (this is a test)",VERSION); 
    
        smartcard_list = fopen(smartcard_list_download,"w");
        
        curl_easy_setopt(curl,CURLOPT_URL,url);
        curl_easy_setopt(curl,CURLOPT_WRITEDATA, smartcard_list);
        curl_easy_setopt(curl,CURLOPT_USERAGENT, user_agent);
        curl_easy_setopt(curl,CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl,CURLOPT_PROGRESSFUNCTION, progress_update_smartcard_list_txt);
        curl_easy_setopt(curl,CURLOPT_PROGRESSDATA, progress);

        res = curl_easy_perform(curl);
        
        fclose(smartcard_list);
        
        if (res!=CURLE_OK)
        {
            log_printf(LOG_ERROR,"Failed to update smartcard_list.txt: %s", curl_easy_strerror(res));
            unlink(smartcard_list_download);
        }
        else
        {
            if (rename(smartcard_list_download,smartcard_list_txt)==0)
            {
                log_printf(LOG_INFO,"Updated smartcard_list.txt");
            }
            else
            {
                /* this should not happen... but you never know */
                log_printf(LOG_ERROR,"Failed to copy smartcard_list.dowload as smartcard_list.txt: %s", strerror(errno));    
                unlink(smartcard_list_download);
            }
            luax_variable_set_integer("cardpeek.smartcard_list.next_update",(int)(now+30*(24*3600)));
        }

        curl_easy_cleanup(curl);

        retval = (res==CURLE_OK);
    }

    gui_inprogress_free(progress);

    luax_config_table_save();
    g_free(url);
    return 0;
}

static int install_dot_file(void)
{
  const char* dot_dir = path_config_get_string(PATH_CONFIG_FOLDER_CARDPEEK);
  const char* old_replay_dir = path_config_get_string(PATH_CONFIG_FOLDER_OLD_REPLAY);
  const char* new_replay_dir = path_config_get_string(PATH_CONFIG_FOLDER_REPLAY);
  const char* home_dir = path_config_get_string(PATH_CONFIG_FOLDER_HOME);
  const char* version_file = path_config_get_string(PATH_CONFIG_FILE_SCRIPT_VERSION);
  GStatBuf sbuf;
  FILE* f;
  int status;
  a_string_t* astr;
  unsigned dot_version=0;
  int response;
  GResource* cardpeek_resources;
  GBytes* dot_cardpeek_tar_gz;
  unsigned char *dot_cardpeek_tar_gz_start;
  gsize dot_cardpeek_tar_gz_size;

  if (g_stat(dot_dir,&sbuf)==0)
  {
    log_printf(LOG_DEBUG,"Found directory '%s'",dot_dir);

    if ((f = g_fopen(version_file,"r"))!=NULL) 
    {
      fscanf(f,"%u",&dot_version);
      fclose(f);
      if (dot_version>=SCRIPT_VERSION) 
      {
	log_printf(LOG_DEBUG,"Scripts are up to date.");
	return 1;
      }
    }
    astr = a_strnew(NULL);
    a_sprintf(astr,"Some scripts in '%s' seem to come from an older version of Cardpeek\n"
	      "Do you want to upgrade these scripts ?",dot_dir);

    if ((response = gui_question(a_strval(astr),"Yes","No","No, don't ask me again",NULL))!=0)
    {
      log_printf(LOG_DEBUG,"The scripts in '%s' will not be upgraded.",dot_dir);
      a_strfree(astr);

      if (response==2)
      {
	if ((f=g_fopen(version_file,"w"))!=NULL)
	{
	  fprintf(f,"%u\n",SCRIPT_VERSION);
	  fclose(f);
	}
      }
      return 0;
    }
    a_strfree(astr);
  }
  else
  {
    astr = a_strnew(NULL);
    a_sprintf(astr,"It seems this is the first time you run Cardpeek, because \n'%s' does not exit (%s).\n"
  	      "Do you want to create '%s' ?",dot_dir,strerror(errno),dot_dir);

    if (gui_question(a_strval(astr),"Yes","No",NULL)!=0)
    {
      log_printf(LOG_DEBUG,"'%s' will not be created",dot_dir);
      a_strfree(astr);

      return 0;
    }
    a_strfree(astr);
  }

  if (g_stat(old_replay_dir,&sbuf)==0)
  {
	if (rename(old_replay_dir,new_replay_dir)==0)
	{
		log_printf(LOG_INFO,"Renamed %s to %s.", 
		           old_replay_dir, new_replay_dir);
	}
	else
	{
		log_printf(LOG_WARNING,"Failed to rename %s to %s: %s",
			   old_replay_dir, new_replay_dir, strerror(errno));
	}
  }

  cardpeek_resources = cardpeek_resources_get_resource();
  if (cardpeek_resources == NULL)
  {
	log_printf(LOG_ERROR,"Could not load cardpeek internal resources. This is not good.");
	return -1; 
  }
  dot_cardpeek_tar_gz = g_resources_lookup_data("/cardpeek/dot_cardpeek.tar.gz",G_RESOURCE_LOOKUP_FLAGS_NONE,NULL);
  if (dot_cardpeek_tar_gz == NULL)
  {
	log_printf(LOG_ERROR,"Could not load .cardpeek.tar.gz");
	return -1;
  }
  dot_cardpeek_tar_gz_start = (unsigned char *)g_bytes_get_data(dot_cardpeek_tar_gz,&dot_cardpeek_tar_gz_size);  
  
  chdir(home_dir);
  if ((f = g_fopen("dot_cardpeek.tar.gz","wb"))==NULL)
  {
	  log_printf(LOG_ERROR,"Could not create dot_cardpeek.tar.gz in %s (%s)", home_dir, strerror(errno));
	  gui_question("Could not create dot_cardpeek.tar.gz, aborting.","Ok",NULL);
	  return 0;
  }
  
  if (fwrite(dot_cardpeek_tar_gz_start,dot_cardpeek_tar_gz_size,1,f)!=1)
  {
	  log_printf(LOG_ERROR,"Could not write to dot_cardpeek.tar.gz in %s (%s)", home_dir, strerror(errno));
	  gui_question("Could not write to dot_cardpeek.tar.gz, aborting.","Ok",NULL);
	  fclose(f);
	  return 0;
  }
  log_printf(LOG_DEBUG,"Wrote %i bytes to dot_cardpeek.tar.gz",dot_cardpeek_tar_gz_size);
  fclose(f);

  g_bytes_unref(dot_cardpeek_tar_gz);

  log_printf(LOG_INFO,"Created dot_cardpeek.tar.gz");
  log_printf(LOG_INFO,"Creating files in %s", home_dir);
  status = system("tar xzvf dot_cardpeek.tar.gz");
  log_printf(LOG_INFO,"'tar xzvf dot_cardpeek.tar.gz' returned %i",status);
  if (status!=0)
  {
	gui_question("Extraction of dot_cardpeek.tar.gz failed, aborting.","Ok",NULL);
	return 0;
  }
  status = system("rm dot_cardpeek.tar.gz");
  log_printf(LOG_INFO,"'rm dot_cardpeek.tar.gz' returned %i",status);

  gui_question("Note: The files have been created.\nIt is recommended that you quit and restart cardpeek, for changes to take effect.","Ok",NULL);
  return 1;
}

static gboolean run_command_from_cli(gpointer data)
{
	luax_run_command((const char *)data);	
	return FALSE;
}

/*
static gboolean run_update_checks(gpointer data)
{
    update_smartcard_list_txt();
	return FALSE;
}
*/

static const char *message = 
"***************************************************************\n"
" Oups...                                                       \n"
"  Cardpeek has encoutered a problem and has exited abnormally. \n"
"  Additionnal information may be available in the file         \n"
"                                                               \n"
"  "
;

static const char *signature =
"\n"
"                                                               \n"
"  L1L1@gmx.com                                                 \n"
"***************************************************************\n"
;

static void save_what_can_be_saved(int sig_num) 
{
  const char *logfile;	
  char buf[32];

  write(2,message,strlen(message));
  logfile = path_config_get_string(PATH_CONFIG_FILE_LOG);
  write(2,logfile,strlen(logfile));
  write(2,signature,strlen(signature));
  sprintf(buf,"Received signal %i\n",sig_num); 
  write(2,buf,strlen(buf));
  log_close_file();
  exit(-2);
} 

static void display_readers_and_version(void)
{
	cardmanager_t *CTX;
	unsigned i;
	unsigned reader_count;
	const char **reader_list;

	log_set_function(NULL);

	luax_init();

	fprintf(stdout,"%sThis is %s.%s\n",ANSI_GREEN,system_string_info(),ANSI_RESET);
	fprintf(stdout,"Cardpeek path is %s\n",path_config_get_string(PATH_CONFIG_FOLDER_CARDPEEK));
	
	CTX = cardmanager_new();
	reader_count = cardmanager_count_readers(CTX);
	reader_list  = cardmanager_reader_name_list(CTX);
	if (reader_count == 0)
		fprintf(stdout,"There are no readers detected\n");
	else if (reader_count==1)
		fprintf(stdout,"There is 1 reader detected:\n");
	else
		fprintf(stdout,"There are %i readers detected:\n",reader_count);
	for (i=0;i<reader_count;i++)
		fprintf(stdout," -> %s\n", reader_list[i]);
	fprintf(stdout,"\n");
	cardmanager_free(CTX);
	luax_release();
}

static void display_help(char *progname)
{
	fprintf(stderr, "Usage: %s [-r|--reader reader-name] [-e|--exec lua-command] [-v|--version]\n",
			progname);
}

static struct option long_options[] = {
            {"reader",  	required_argument, 0,  'r' },
            {"exec",    	required_argument, 0,  'e' },
            {"version", 	no_argument, 	   0,  'v' },
            {"help", 		no_argument, 	   0,  'h' },
            {0,        	 	0,                 0,   0 }
        };

int main(int argc, char **argv)
{
  cardmanager_t* CTX;
  cardreader_t* READER;
  int opt;
  int opt_index = 0;
  int run_gui = 1;
  char* reader_name = NULL;
  char* exec_command = NULL;

  signal(SIGSEGV, save_what_can_be_saved); 
  
  path_config_init();
    
  log_open_file();

  while ((opt = getopt_long(argc,argv,"r:e:vh",long_options,&opt_index))!=-1) 
  {
	  switch (opt) {
		case 'r':
			reader_name = g_strdup(optarg);
                        break;
		case 'e':
			exec_command = optarg;
			break;
		case 'v':
			display_readers_and_version();
			run_gui = 0;
			break;
		default:
			display_help(argv[0]);
			run_gui = 0;
	  }
  }
   
  if (run_gui)
  {
      /* if we want threads: 
         gdk_threads_init(); 
         gdk_threads_enter();
       */

      gui_init(&argc,&argv);

	  gui_create();

	  log_printf(LOG_INFO,"Running %s",system_string_info());

	  install_dot_file(); 

	  luax_init();


	  CTX = cardmanager_new();

	  if (reader_name == NULL)
	  {
		  reader_name = gui_select_reader(cardmanager_count_readers(CTX),
				                  cardmanager_reader_name_list(CTX));
	  }

	  READER = cardreader_new(reader_name);

	  cardmanager_free(CTX);

	  if (READER)
	  {
		  luax_set_card_reader(READER);

		  cardreader_set_callback(READER,gui_readerview_print,NULL);

		  if (exec_command) 
                g_idle_add(run_command_from_cli,exec_command);
          else
                update_smartcard_list_txt();
          /*else
                g_idle_add(run_update_checks,NULL);
            */
		  gui_run();

		  cardreader_free(READER);
	  }
	  else
	  {
		  fprintf(stderr,"Failed to open smart card reader '%s'.\n",reader_name);
		  log_printf(LOG_ERROR,"Failed to open smart card reader '%s'.", reader_name);
	  }

	  luax_config_table_save();

	  luax_release();
        
      /* if we want threads:
         gdk_threads_leave();
       */
  }

  if (reader_name) g_free(reader_name);
  
  log_close_file();

  path_config_release();  

  return 0;
}


