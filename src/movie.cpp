#include <assert.h>
#include <limits.h>
#include <fstream>
#ifdef WIN32
#else
#include "types.h"
#endif
//#include "utils/guid.h"
#include "utils/xstring.h"
#include "movie.h"

extern void PCE_Power();

#include "utils/readwrite.h"

using namespace std;
bool freshMovie = false;	  //True when a movie loads, false when movie is altered.  Used to determine if a movie has been altered since opening
bool autoMovieBackup = true;

#define FCEU_PrintError LOG

#define MOVIE_VERSION 1
#define DESMUME_VERSION_NUMERIC 9

//----movie engine main state
extern bool PCE_IsCD;

EMOVIEMODE movieMode = MOVIEMODE_INACTIVE;

//this should not be set unless we are in MOVIEMODE_RECORD!
fstream* osRecordingMovie = 0;

//////////////////////////////////////////////////////////////////////////////

int LagFrameFlag;
int FrameAdvanceVariable=0;
int currFrameCounter;
char MovieStatusM[40];
int LagFrameCounter;
extern "C" {
int AutoAdvanceLag;
}
//////////////////////////////////////////////////////////////////////////////

uint32 cur_input_display = 0;
int pauseframe = -1;
bool movie_readonly = true;

char curMovieFilename[512] = {0};
MovieData currMovieData;
int currRerecordCount;

//--------------
#include "mednafen.h"

void DisplayMessage(char* str) {}

extern void DisplayMessage(char* str);

//adelikat: Adding this specifically for the crappy yabause OSD, to return a string of /xxxx to add to a movie if in playback mode
char* GetMovieLengthStr()
{
	char str[10] = "";
	int length;

	if (MovieIsActive() && movie_readonly)
	{
		length = (int)currMovieData.records.size();
		sprintf(str, "/%d", length);
	}

	return str;
}

void MovieData::clearRecordRange(int start, int len)
{
	for(int i=0;i<len;i++)
		records[i+start].clear();
}

void MovieData::insertEmpty(int at, int frames)
{
	if(at == -1) 
	{
		int currcount = records.size();
		records.resize(records.size()+frames);
		clearRecordRange(currcount,frames);
	}
	else
	{
		records.insert(records.begin()+at,frames,MovieRecord());
		clearRecordRange(at,frames);
	}
}



void MovieRecord::clear()
{ 
	for (int i = 0; i < 5; i++)
		pad[i] = 0;
	commands = 0;
	touch.padding = 0;
}


//const char MovieRecord::mnemonics[13] = {'R','L','D','U','T','S','B','A','Y','X','W','E','G'};
const char MovieRecord::mnemonics[8] = {'U','D','L','R','1','2','N','S'};
void MovieRecord::dumpPad(std::ostream* os, uint16 pad)
{
	//these are mnemonics for each joystick bit.
	//since we usually use the regular joypad, these will be more helpful.
	//but any character other than ' ' or '.' should count as a set bit
	//maybe other input types will need to be encoded another way..
	for(int bit=0;bit<8;bit++)
	{
		int bitmask = (1<<(7-bit));
		char mnemonic = mnemonics[bit];
		//if the bit is set write the mnemonic
		if(pad & bitmask)
			os->put(mnemonic);
		else //otherwise write an unset bit
			os->put('.');
	}
}
#undef read
#undef open
#undef close
#define read read
#define open open
#define close close


void MovieRecord::parsePad(std::istream* is, uint16& pad)
{
	char buf[8];
	is->read(buf,8);
	pad = 0;
	for(int i=0;i<8;i++)
	{
		pad <<= 1;
		pad |= ((buf[i]=='.'||buf[i]==' ')?0:1);
	}
}


void MovieRecord::parse(MovieData* md, std::istream* is)
{
	//by the time we get in here, the initial pipe has already been extracted

	//extract the commands
	commands = u32DecFromIstream(is);
	
	is->get(); //eat the pipe

	for (int i = 0; i < md->ports; i++) {
		parsePad(is, pad[i]);
		is->get();
	}
//	touch.x = u32DecFromIstream(is);
//	touch.y = u32DecFromIstream(is);
//	touch.touch = u32DecFromIstream(is);
		
	is->get(); //eat the pipe

	//should be left at a newline
}


void MovieRecord::dump(MovieData* md, std::ostream* os, int index)
{
	//dump the misc commands
	//*os << '|' << setw(1) << (int)commands;
	os->put('|');
	putdec<uint8,1,true>(os,commands);

	os->put('|');
//	dumpPad(os, pad);
	for (int i = 0; i < md->ports; i++) 
	{
		dumpPad(os,pad[i]); 
		os->put('|');
	}
//	putdec<u8,3,true>(os,touch.x); os->put(' ');
//	putdec<u8,3,true>(os,touch.y); os->put(' ');
//	putdec<u8,1,true>(os,touch.touch);
//	os->put('|');
	
	//each frame is on a new line
	os->put('\n');
}

MovieData::MovieData()
	: version(MOVIE_VERSION)
	, emuVersion(DESMUME_VERSION_NUMERIC)
	, rerecordCount(1)
	, binaryFlag(false)
	//, greenZoneCount(0)
{
//	memset(&romChecksum,0,sizeof(MD5DATA));
}

void MovieData::truncateAt(int frame)
{
	records.resize(frame);
}


void MovieData::installValue(std::string& key, std::string& val)
{
	//todo - use another config system, or drive this from a little data structure. because this is gross
	if(key == "PCECD")
		installInt(val,pcecd);
	else if(key == "version")
		installInt(val,version);
	else if(key == "emuVersion")
		installInt(val,emuVersion);
	else if(key == "rerecordCount")
		installInt(val,rerecordCount);
	else if(key == "romFilename")
		romFilename = val;
	else if(key == "ports")
		installInt(val, ports);
//	else if(key == "romChecksum")
//		StringToBytes(val,&romChecksum,MD5DATA::size);
//	else if(key == "guid")
//		guid = Desmume_Guid::fromString(val);
//	else if(key == "comment")
//		comments.push_back(mbstowcs(val));
	else if(key == "binary")
		installBool(val,binaryFlag);
	else if(key == "savestate")
	{
/*		int len = Base64StringToBytesLength(val);
		if(len == -1) len = HexStringToBytesLength(val); // wasn't base64, try hex
		if(len >= 1)
		{
			savestate.resize(len);
			StringToBytes(val,&savestate[0],len); // decodes either base64 or hex
		}
	}*/
}
}

char* RemovePath(char * input) {

	char* temp=(char*)malloc(1024);
	strcpy(temp, input);

	if (strrchr(temp, '/'))
        temp = 1 + strrchr(temp, '/');

	if (strrchr(temp, '\\'))
        temp = 1 + strrchr(temp, '\\');

	return temp;
}


int MovieData::dump(std::ostream *os, bool binary)
{
	int start = os->tellp();
	*os << "version " << version << endl;
	*os << "emuVersion " << emuVersion << endl;
	*os << "rerecordCount " << rerecordCount << endl;
	*os << "ports " << ports << endl;
	*os << "PCECD " << PCE_IsCD << endl;
/*	*os << "cdGameName " << cdip->gamename << endl;
	*os << "cdInfo " << cdip->cdinfo << endl;
	*os << "cdItemNum " << cdip->itemnum << endl;
	*os << "cdVersion " << cdip->version << endl;
	*os << "cdDate " << cdip->date << endl;
	*os << "cdRegion " << cdip->region << endl;
	*os << "emulatedBios " << yabsys.emulatebios << endl;
	*os << "isPal " << yabsys.IsPal << endl;
#ifdef WIN32
	*os << "sh2CoreType " << int(sh2coretype) << endl;
	*os << "sndCoreType " << int(sndcoretype) << endl;
	*os << "vidCoreType " << int(vidcoretype) << endl;
	*os << "cartType " << carttype << endl;

	*os << "cdRomPath " << RemovePath(cdrompath) << endl;
	*os << "biosFilename " << RemovePath(biosfilename) << endl;
	*os << "cartFilename " << RemovePath(cartfilename) << endl;
#endif
*/
//	fwrite("YMV", sizeof("YMV"), 1, fp);
//	fwrite(VERSION, sizeof(VERSION), 1, fp);

	////
//	*os << "romChecksum " << BytesToString(romChecksum.data,MD5DATA::size) << endl;
//	*os << "guid " << guid.toString() << endl;

//	for(u32 i=0;i<comments.size();i++)
//		*os << "comment " << wcstombs(comments[i]) << endl;
	
	if(binary)
		*os << "binary 1" << endl;
		
//	if(savestate.size() != 0)
//		*os << "savestate " << BytesToString(&savestate[0],savestate.size()) << endl;
	if(binary)
	{
		//put one | to start the binary dump
		os->put('|');
		for(int i=0;i<(int)records.size();i++)
			records[i].dumpBinary(this,os,i);
	}
	else
		for(int i=0;i<(int)records.size();i++)
			records[i].dump(this,os,i);

	int end = os->tellp();
	return end-start;
}

//yuck... another custom text parser.
bool LoadFM2(MovieData& movieData, std::istream* fp, int size, bool stopAfterHeader)
{
	//TODO - start with something different. like 'desmume movie version 1"
	std::ios::pos_type curr = fp->tellg();

	//movie must start with "version 1"
	char buf[9];
	curr = fp->tellg();
	fp->read(buf,9);
	fp->seekg(curr);
	if(fp->fail()) return false;
	if(memcmp(buf,"version 1",9)) 
		return false;

	std::string key,value;
	enum {
		NEWLINE, KEY, SEPARATOR, VALUE, RECORD, COMMENT
	} state = NEWLINE;
	bool bail = false;
	for(;;)
	{
		bool iswhitespace, isrecchar, isnewline;
		int c;
		if(size--<=0) goto bail;
		c = fp->get();
		if(c == -1)
			goto bail;
		iswhitespace = (c==' '||c=='\t');
		isrecchar = (c=='|');
		isnewline = (c==10||c==13);
		if(isrecchar && movieData.binaryFlag && !stopAfterHeader)
		{
			LoadFM2_binarychunk(movieData, fp, size);
			return true;
		}
		switch(state)
		{
		case NEWLINE:
			if(isnewline) goto done;
			if(iswhitespace) goto done;
			if(isrecchar) 
				goto dorecord;
			//must be a key
			key = "";
			value = "";
			goto dokey;
			break;
		case RECORD:
			{
				dorecord:
				if (stopAfterHeader) return true;
				int currcount = movieData.records.size();
				movieData.records.resize(currcount+1);
				int preparse = fp->tellg();
				movieData.records[currcount].parse(&movieData, fp);
				int postparse = fp->tellg();
				size -= (postparse-preparse);
				state = NEWLINE;
				break;
			}

		case KEY:
			dokey: //dookie
			state = KEY;
			if(iswhitespace) goto doseparator;
			if(isnewline) goto commit;
			key += c;
			break;
		case SEPARATOR:
			doseparator:
			state = SEPARATOR;
			if(isnewline) goto commit;
			if(!iswhitespace) goto dovalue;
			break;
		case VALUE:
			dovalue:
			state = VALUE;
			if(isnewline) goto commit;
			value += c;
			break;
		case COMMENT:
		default:
			break;
		}
		goto done;

		bail:
		bail = true;
		if(state == VALUE) goto commit;
		goto done;
		commit:
		movieData.installValue(key,value);
		state = NEWLINE;
		done: ;
		if(bail) break;
	}

	return true;
}


static void closeRecordingMovie()
{
	if(osRecordingMovie)
	{
		delete osRecordingMovie;
		osRecordingMovie = 0;
	}
}

/// Stop movie playback.
static void StopPlayback()
{
	DisplayMessage("Movie playback stopped.");
	strcpy(MovieStatusM, "");
	movieMode = MOVIEMODE_INACTIVE;
}


/// Stop movie recording
static void StopRecording()
{
	DisplayMessage("Movie recording stopped.");
	strcpy(MovieStatusM, "");
	movieMode = MOVIEMODE_INACTIVE;
	
	closeRecordingMovie();
}



void FCEUI_StopMovie()
{
	if(movieMode == MOVIEMODE_PLAY)
		StopPlayback();
	else if(movieMode == MOVIEMODE_RECORD)
		StopRecording();

	curMovieFilename[0] = 0;
	freshMovie = false;
}
	extern void PCE_Power(void);

extern uint8 SaveRAM[2048];


//begin playing an existing movie
void FCEUI_LoadMovie(const char *fname, bool _read_only, bool tasedit, int _pauseframe)
{
	//if(!tasedit && !FCEU_IsValidUI(FCEUI_PLAYMOVIE))
	//	return;

	assert(fname);

	//mbg 6/10/08 - we used to call StopMovie here, but that cleared curMovieFilename and gave us crashes...
	if(movieMode == MOVIEMODE_PLAY)
		StopPlayback();
	else if(movieMode == MOVIEMODE_RECORD)
		StopRecording();
	//--------------

	currMovieData = MovieData();
	
	strcpy(curMovieFilename, fname);
	//FCEUFILE *fp = FCEU_fopen(fname,0,"rb",0);
	//if (!fp) return;
	//if(fp->isArchive() && !_read_only) {
	//	FCEU_PrintError("Cannot open a movie in read+write from an archive.");
	//	return;
	//}

	//LoadFM2(currMovieData, fp->stream, INT_MAX, false);

	currMovieData.ports = 1;	
	
	fstream fs (fname);
	LoadFM2(currMovieData, &fs, INT_MAX, false);
	fs.close();

	//TODO
	//fully reload the game to reinitialize everything before playing any movie
	//poweron(true);

//	extern bool _HACK_DONT_STOPMOVIE;
//	_HACK_DONT_STOPMOVIE = true;
	PCE_Power();
//	_HACK_DONT_STOPMOVIE = false;
	////WE NEED TO LOAD A SAVESTATE
	//if(currMovieData.savestate.size() != 0)
	//{
	//	bool success = MovieData::loadSavestateFrom(&currMovieData.savestate);
	//	if(!success) return;
	//}

	currFrameCounter = 0;
	pauseframe = _pauseframe;
	movie_readonly = _read_only;
	movieMode = MOVIEMODE_PLAY;
	currRerecordCount = currMovieData.rerecordCount;
	MovClearAllSRAM();
//	InitMovieTime();
	freshMovie = true;
//	ClearAutoHold();
	LagFrameCounter=0;

	if(movie_readonly)
		DisplayMessage("Replay started Read-Only.");
	//	driver->USR_InfoMessage("Replay started Read-Only.");
	else
		DisplayMessage("Replay started Read+Write.");
	//	driver->USR_InfoMessage("Replay started Read+Write.");
}

static void openRecordingMovie(const char* fname)
{
	//osRecordingMovie = FCEUD_UTF8_fstream(fname, "wb");
	osRecordingMovie = new fstream(fname,std::ios_base::out);
	/*if(!osRecordingMovie)
		FCEU_PrintError("Error opening movie output file: %s",fname);*/
	strcpy(curMovieFilename, fname);
}


//begin recording a new movie
//TODO - BUG - the record-from-another-savestate doesnt work.
 void FCEUI_SaveMovie(const char *fname, std::wstring author)
{
	//if(!FCEU_IsValidUI(FCEUI_RECORDMOVIE))
	//	return;

	assert(fname);

	FCEUI_StopMovie();

	openRecordingMovie(fname);

	currFrameCounter = 0;
	//LagCounterReset();

	currMovieData = MovieData();
//	currMovieData.guid.newGuid();

	if(author != L"") currMovieData.comments.push_back(L"author " + author);
	//currMovieData.romChecksum = GameInfo->MD5;
//	currMovieData.romFilename = cdip->gamename;//GetRomName();
	
//	extern bool _HACK_DONT_STOPMOVIE;
//	_HACK_DONT_STOPMOVIE = true;
//	NDS_Reset();
#ifdef WIN32
	//HardResetGame();
#else
//	YabauseReset();
#endif
	MovClearAllSRAM();
	MDFNI_Power();
//	_HACK_DONT_STOPMOVIE = false;

	//todo ?
	//poweron(true);
	//else
	//	MovieData::dumpSavestateTo(&currMovieData.savestate,Z_BEST_COMPRESSION);

	//we are going to go ahead and dump the header. from now on we will only be appending frames
	currMovieData.dump(osRecordingMovie, false);

	movieMode = MOVIEMODE_RECORD;
	movie_readonly = false;
	currRerecordCount = 0;
//	InitMovieTime();
//	MovieSRAM();
//	BupFormat(0);
	LagFrameCounter=0;

	DisplayMessage("Movie recording started.");
//	driver->USR_InfoMessage("Movie recording started.");
}

 void NDS_setTouchFromMovie(void) {

/*	 if(movieMode == MOVIEMODE_PLAY)
	 {

		 MovieRecord* mr = &currMovieData.records[currFrameCounter];
		 nds.touchX=mr->touch.x << 4;
		 nds.touchY=mr->touch.y << 4;

		 if(mr->touch.touch) {
			 nds.isTouch=mr->touch.touch;
			 MMU.ARM7_REG[0x136] &= 0xBF;
		 }
		 else {
			 nds.touchX=0;
			 nds.touchY=0;
			 nds.isTouch=0;

			 MMU.ARM7_REG[0x136] |= 0x40;
		 }
		 osd->addFixed(mr->touch.x, mr->touch.y, "%s", "X");
	 }*/
 }
#define MDFNNPCMD_RESET 	0x01
#define MDFNNPCMD_POWER 	0x02

extern uint16 pcepad;

void NDS_setPadFromMovie(uint16 pad[])
{
	for (int i = 0; i < currMovieData.ports; i++) {
	pcepad = 0;

	if(pad[i] &(1 << 7)) pcepad |= (1 << 4);//u
	if(pad[i] &(1 << 6)) pcepad |= (1 << 6);//d
	if(pad[i] &(1 << 5)) pcepad |= (1 << 7);//l
	if(pad[i] &(1 << 4)) pcepad |= (1 << 5);//r
	if(pad[i] &(1 << 3)) pcepad |= (1 << 0);//o
	if(pad[i] &(1 << 2)) pcepad |= (1 << 1);//t
	if(pad[i] &(1 << 0)) pcepad |= (1 << 2);//s
	if(pad[i] &(1 << 1)) pcepad |= (1 << 3);//n

	}

}
 static int _currCommand = 0;
extern void *PortDataCache[16];
 //the main interaction point between the emulator and the movie system.
 //either dumps the current joystick state or loads one state from the movie
void FCEUMOV_AddInputState()
 {
//	 uint16 pad;
	 //todo - for tasedit, either dump or load depending on whether input recording is enabled
	 //or something like that
	 //(input recording is just like standard read+write movie recording with input taken from gamepad)
	 //otherwise, it will come from the tasedit data.

	 if(LagFrameFlag == 1)
		 LagFrameCounter++;
	 LagFrameFlag=1;

	 if(movieMode == MOVIEMODE_PLAY)
	 {
		 //stop when we run out of frames
		 if(currFrameCounter >= (int)currMovieData.records.size())
		 {
			 StopPlayback();
		 }
		 else
		 {

			 strcpy(MovieStatusM, "Playback");

			 MovieRecord* mr = &currMovieData.records[currFrameCounter];

			 //reset if necessary
			 if(mr->command_reset())
				 MDFN_DoSimpleCommand(MDFNNPCMD_RESET);

			 if(mr->command_power())
				 MDFN_DoSimpleCommand(MDFNNPCMD_POWER);

			// {}
			 //ResetNES();
			 NDS_setPadFromMovie(mr->pad);
//			 NDS_setTouchFromMovie();
		 }

		 //if we are on the last frame, then pause the emulator if the player requested it
		 if(currFrameCounter == (int)currMovieData.records.size()-1)
		 {
			 /*if(FCEUD_PauseAfterPlayback())
			 {
			 FCEUI_ToggleEmulationPause();
			 }*/
		 }

		 //pause the movie at a specified frame 
		 //if(FCEUMOV_ShouldPause() && FCEUI_EmulationPaused()==0)
		 //{
		 //	FCEUI_ToggleEmulationPause();
		 //	FCEU_DispMessage("Paused at specified movie frame");
		 //}
//		 osd->addFixed(180, 176, "%s", "Playback");

	 }
	 else if(movieMode == MOVIEMODE_RECORD)
	 {
		 MovieRecord mr;
		mr.commands = _currCommand;
		_currCommand = 0;

	//	 mr.commands = 0;

		 strcpy(MovieStatusM, "Recording");

		 int II, I, n, s, u, r, d, l;

		 for (int i = 0; i < currMovieData.ports; i++) {

			

#define FIX(b) (b?1:0)
			 II = FIX(pcepad &(1 << 1));
			 I = FIX(pcepad & (1 << 0));
			 n = FIX(pcepad & (1 << 2));
			 s = FIX(pcepad & (1 << 3));
			 u = FIX(pcepad & (1 << 4));
			 r = FIX(pcepad & (1 << 7));
			 d = FIX(pcepad & (1 << 6));
			 l = FIX(pcepad & (1 << 5));

			 mr.pad[i] =
				 (FIX(r)<<5)|
				 (FIX(l)<<4)|
				 (FIX(u)<<7)|
				 (FIX(d)<<6)|
				 (FIX(I)<<3)|
				 (FIX(II)<<2)|
				 (FIX(s)<<1)|
				 (FIX(n)<<0);
		 }

		 mr.dump(&currMovieData, osRecordingMovie,currMovieData.records.size());
		 currMovieData.records.push_back(mr);
//		 osd->addFixed(180, 176, "%s", "Recording");
	 }

	 currFrameCounter++;

	 /*extern u8 joy[4];
	 memcpy(&cur_input_display,joy,4);*/
 }


//TODO 
static void FCEUMOV_AddCommand(int cmd)
{
	// do nothing if not recording a movie
	if(movieMode != MOVIEMODE_RECORD)
		return;
	
	//printf("%d\n",cmd);

	//MBG TODO BIG TODO TODO TODO
	//DoEncode((cmd>>3)&0x3,cmd&0x7,1);
}

//little endian 4-byte cookies
static const int kMOVI = 0x49564F4D;
static const int kNOMO = 0x4F4D4F4E;

#define LOCAL_LE

int read32lemov(int *Bufo, std::istream *is)
{
	uint32 buf;
	if(is->read((char*)&buf,4).gcount() != 4)
		return 0;
#ifdef LOCAL_LE
	*(uint32*)Bufo=buf;
#else
	*(u32*)Bufo=((buf&0xFF)<<24)|((buf&0xFF00)<<8)|((buf&0xFF0000)>>8)|((buf&0xFF000000)>>24);
#endif
	return 1;
}


int write32lemov(uint32 b, std::ostream* os)
{
	uint8 s[4];
	s[0]=b;
	s[1]=b>>8;
	s[2]=b>>16;
	s[3]=b>>24;
	os->write((char*)&s,4);
	return 4;
}

void mov_savestate(std::ostream *os)//std::ostream* os
{
	//we are supposed to dump the movie data into the savestate
	//if(movieMode == MOVIEMODE_RECORD || movieMode == MOVIEMODE_PLAY)
	//	return currMovieData.dump(os, true);
	//else return 0;
	if(movieMode == MOVIEMODE_RECORD || movieMode == MOVIEMODE_PLAY)
	{
		write32lemov(kMOVI,os);
		currMovieData.dump(os, true);
	}
	else
	{
		write32lemov(kNOMO,os);
	}
}

static bool load_successful;

bool mov_loadstate(std::istream* is, int size)//std::istream* is
{
	load_successful = false;

	int cookie;
	if(read32lemov(&cookie,is) != 1) return false;
	if(cookie == kNOMO)
		return true;
	else if(cookie != kMOVI)
		return false;

	size -= 4;

	if (!movie_readonly && autoMovieBackup && freshMovie) //If auto-backup is on, movie has not been altered this session and the movie is in read+write mode
	{
		FCEUI_MakeBackupMovie(false);	//Backup the movie before the contents get altered, but do not display messages						  
	}

	//a little rule: cant load states in read+write mode with a movie from an archive.
	//so we are going to switch it to readonly mode in that case
//	if(!movie_readonly 
//		//*&& FCEU_isFileInArchive(curMovieFilename)*/
//		) {
//		FCEU_PrintError("Cannot loadstate in Read+Write with movie from archive. Movie is now Read-Only.");
//		movie_readonly = true;
//	}

	MovieData tempMovieData = MovieData();
	std::ios::pos_type curr = is->tellg();
	if(!LoadFM2(tempMovieData, is, size, false)) {
		
	//	is->seekg((u32)curr+size);
	/*	extern bool FCEU_state_loading_old_format;
		if(FCEU_state_loading_old_format) {
			if(movieMode == MOVIEMODE_PLAY || movieMode == MOVIEMODE_RECORD) {
				FCEUI_StopMovie();			
				FCEU_PrintError("You have tried to use an old savestate while playing a movie. This is unsupported (since the old savestate has old-format movie data in it which can't be converted on the fly)");
			}
		}*/
		return false;
	}

	//complex TAS logic for when a savestate is loaded:
	//----------------
	//if we are playing or recording and toggled read-only:
	//  then, the movie we are playing must match the guid of the one stored in the savestate or else error.
	//  the savestate is assumed to be in the same timeline as the current movie.
	//  if the current movie is not long enough to get to the savestate's frame#, then it is an error. 
	//  the movie contained in the savestate will be discarded.
	//  the emulator will be put into play mode.
	//if we are playing or recording and toggled read+write
	//  then, the movie we are playing must match the guid of the one stored in the savestate or else error.
	//  the movie contained in the savestate will be loaded into memory
	//  the frames in the movie after the savestate frame will be discarded
	//  the in-memory movie will have its rerecord count incremented
	//  the in-memory movie will be dumped to disk as fcm.
	//  the emulator will be put into record mode.
	//if we are doing neither:
	//  then, we must discard this movie and just load the savestate


	if(movieMode == MOVIEMODE_PLAY || movieMode == MOVIEMODE_RECORD)
	{
		//handle moviefile mismatch
/*		if(tempMovieData.guid != currMovieData.guid)
		{
			//mbg 8/18/08 - this code  can be used to turn the error message into an OK/CANCEL
			#ifdef WIN32
				//std::string msg = "There is a mismatch between savestate's movie and current movie.\ncurrent: " + currMovieData.guid.toString() + "\nsavestate: " + tempMovieData.guid.toString() + "\n\nThis means that you have loaded a savestate belonging to a different movie than the one you are playing now.\n\nContinue loading this savestate anyway?";
				//extern HWND pwindow;
				//int result = MessageBox(pwindow,msg.c_str(),"Error loading savestate",MB_OKCANCEL);
				//if(result == IDCANCEL)
				//	return false;
			#else
				FCEU_PrintError("Mismatch between savestate's movie and current movie.\ncurrent: %s\nsavestate: %s\n",currMovieData.guid.toString().c_str(),tempMovieData.guid.toString().c_str());
				return false;
			#endif
		}*/

		closeRecordingMovie();

		if(movie_readonly)
		{
			//-------------------------------------------------------------
			//this code would reload the movie from disk. allegedly it is helpful to hexers, but
			//it is way too slow with dsm format. so it is only here as a reminder, and in case someone
			//wants to play with it
			//-------------------------------------------------------------
			//{
			//	fstream fs (curMovieFilename);
			//	if(!LoadFM2(tempMovieData, &fs, INT_MAX, false))
			//	{
			//		FCEU_PrintError("Failed to reload DSM after loading savestate");
			//	}
			//	fs.close();
			//	currMovieData = tempMovieData;
			//}
			//-------------------------------------------------------------

			//if the frame counter is longer than our current movie, then error
			if(currFrameCounter > (int)currMovieData.records.size())
			{
//				FCEU_PrintError("Savestate is from a frame (%d) after the final frame in the movie (%d). This is not permitted.", currFrameCounter, currMovieData.records.size()-1);
				return false;
			}

			movieMode = MOVIEMODE_PLAY;
		}
		else
		{
			//truncate before we copy, just to save some time
			tempMovieData.truncateAt(currFrameCounter);
			currMovieData = tempMovieData;
			
		//	#ifdef _S9XLUA_H
		//	if(!FCEU_LuaRerecordCountSkip())
				currRerecordCount++;
		//	#endif
			
			currMovieData.rerecordCount = currRerecordCount;

			openRecordingMovie(curMovieFilename);
			//printf("DUMPING MOVIE: %d FRAMES\n",currMovieData.records.size());
			currMovieData.dump(osRecordingMovie, false);
			movieMode = MOVIEMODE_RECORD;
		}
	}
	
	load_successful = true;
	freshMovie = false;

	//// Maximus: Show the last input combination entered from the
	//// movie within the state
	//if(current!=0) // <- mz: only if playing or recording a movie
	//	memcpy(&cur_input_display, joop, 4);

	return true;
}

void SaveStateMovie(char* filename) {

	strcat (filename, "mov");
	filebuf fb;
	fb.open (filename, ios::out | ios::binary);//ios::out
	ostream os(&fb);
	mov_savestate(&os);
	fb.close();
}

void LoadStateMovie(char* filename) {

	std::string fname = (std::string)filename + "mov";

    FILE * fp = fopen( fname.c_str(), "r" );
	if(!fp)
		return;
	
	fseek( fp, 0, SEEK_END );
	int size = ftell(fp);
	fclose(fp);

	filebuf fb;
	fb.open (fname.c_str(), ios::in | ios::binary);//ios::in
	istream is(&fb);
	mov_loadstate(&is, size);
	fb.close();
}

static void FCEUMOV_PreLoad(void)
{
	load_successful=0;
}

static bool FCEUMOV_PostLoad(void)
{
	if(movieMode == MOVIEMODE_INACTIVE)
		return true;
	else
		return load_successful;
}


bool FCEUI_MovieGetInfo(std::istream* fp, MOVIE_INFO& info, bool skipFrameCount)
{
	//MovieData md;
	//if(!LoadFM2(md, fp, INT_MAX, skipFrameCount))
	//	return false;
	//
	//info.movie_version = md.version;
	//info.poweron = md.savestate.size()==0;
	//info.pal = md.palFlag;
	//info.nosynchack = true;
	//info.num_frames = md.records.size();
	//info.md5_of_rom_used = md.romChecksum;
	//info.emu_version_used = md.emuVersion;
	//info.name_of_rom_used = md.romFilename;
	//info.rerecord_count = md.rerecordCount;
	//info.comments = md.comments;

	return true;
}

bool MovieRecord::parseBinary(MovieData* md, std::istream* is)
{
	commands=is->get();
		
	for (int i = 0; i < md->ports; i++)
	{
		is->read((char *) &pad[i], sizeof pad[i]);

	//	parseJoy(is,pad[0]); is->get(); //eat the pipe
	//	parseJoy(is,joysticks[1]); is->get(); //eat the pipe
//		parseJoy(is,joysticks[2]); is->get(); //eat the pipe
//		parseJoy(is,joysticks[3]); is->get(); //eat the pipe
	}
//	else
//		is->read((char *) &pad, sizeof pad);
//	is->read((char *) &touch.x, sizeof touch.x);
//	is->read((char *) &touch.y, sizeof touch.y);
//	is->read((char *) &touch.touch, sizeof touch.touch);
	return true;
}


void MovieRecord::dumpBinary(MovieData* md, std::ostream* os, int index)
{
	os->put(md->records[index].commands);
	for (int i = 0; i < md->ports; i++) {
	os->write((char *) &md->records[index].pad[i], sizeof md->records[index].pad[i]);
	}
//	os->write((char *) &md->records[index].touch.x, sizeof md->records[index].touch.x);
//	os->write((char *) &md->records[index].touch.y, sizeof md->records[index].touch.y);
//	os->write((char *) &md->records[index].touch.touch, sizeof md->records[index].touch.touch);
}

void LoadFM2_binarychunk(MovieData& movieData, std::istream* fp, int size)
{
	int recordsize = 1; //1 for the command

	recordsize = 3;

	assert(size%3==0);

	//find out how much remains in the file
	int curr = fp->tellg();
	fp->seekg(0,std::ios::end);
	int end = fp->tellg();
	int flen = end-curr;
	fp->seekg(curr,std::ios::beg);

#undef min
	//the amount todo is the min of the limiting size we received and the remaining contents of the file
	int todo = std::min(size, flen);

	int numRecords = todo/recordsize;
	//printf("LOADED MOVIE: %d records; currFrameCounter: %d\n",numRecords,currFrameCounter);
	movieData.records.resize(numRecords);
	for(int i=0;i<numRecords;i++)
	{
		movieData.records[i].parseBinary(&movieData,fp);
	}
}

#include <sstream>

bool CheckFileExists(const char* filename)
{
	//This function simply checks to see if the given filename exists
	string checkFilename; 

	if (filename)
		checkFilename = filename;
			
	//Check if this filename exists
	fstream test;
	test.open(checkFilename.c_str(),fstream::in);
		
	if (test.fail())
	{
		test.close();
		return false; 
	}
	else
	{
		test.close();
		return true; 
	}
}

void FCEUI_MakeBackupMovie(bool dispMessage)
{
	//This function generates backup movie files
	string currentFn;					//Current movie fillename
	string backupFn;					//Target backup filename
	string tempFn;						//temp used in back filename creation
	stringstream stream;
	int x;								//Temp variable for string manip
	bool exist = false;					//Used to test if filename exists
	bool overflow = false;				//Used for special situation when backup numbering exceeds limit

	currentFn = curMovieFilename;		//Get current moviefilename
	backupFn = curMovieFilename;		//Make backup filename the same as current moviefilename
	x = backupFn.find_last_of(".");		 //Find file extension
	backupFn = backupFn.substr(0,x);	//Remove extension
	tempFn = backupFn;					//Store the filename at this point
	for (unsigned int backNum=0;backNum<999;backNum++) //999 = arbituary limit to backup files
	{
		stream.str("");					 //Clear stream
		if (backNum > 99)
			stream << "-" << backNum;	 //assign backNum to stream
		 else if (backNum <= 99 && backNum >= 10)
			stream << "-0" << backNum;      //Make it 010, etc if two digits
		else
			stream << "-00" << backNum;	 //Make it 001, etc if single digit
		backupFn.append(stream.str());	 //add number to bak filename
		backupFn.append(".bak");		 //add extension

		exist = CheckFileExists(backupFn.c_str());	//Check if file exists
		
		if (!exist) 
			break;						//Yeah yeah, I should use a do loop or something
		else
		{
			backupFn = tempFn;			//Before we loop again, reset the filename
			
			if (backNum == 999)			//If 999 exists, we have overflowed, let's handle that
			{
				backupFn.append("-001.bak"); //We are going to simply overwrite 001.bak
				overflow = true;		//Flag that we have exceeded limit
				break;					//Just in case
			}
		}
	}

	MovieData md = currMovieData;								//Get current movie data
	std::fstream* outf = new fstream(backupFn.c_str(),std::ios_base::out); //FCEUD_UTF8_fstream(backupFn, "wb");	//open/create file
	md.dump(outf,false);										//dump movie data
	delete outf;												//clean up, delete file object
	
	//TODO, decide if fstream successfully opened the file and print error message if it doesn't

	if (dispMessage)	//If we should inform the user 
	{
//		if (overflow)
//			FCEUI_DispMessage("Backup overflow, overwriting %s",backupFn.c_str()); //Inform user of overflow
//		else
//			FCEUI_DispMessage("%s created",backupFn.c_str()); //Inform user of backup filename
	}
}

void ToggleReadOnly() {

	movie_readonly ^=1;

	if(movie_readonly==1) DisplayMessage("Movie is now read only.");
	else DisplayMessage("Movie is now read+write.");

}

int MovieIsActive() {

	if(movieMode == MOVIEMODE_INACTIVE)
		return false;
	
	return true;
}

void MakeMovieStateName(const char *filename) {

	if(movieMode != MOVIEMODE_INACTIVE)
		strcat ((char *)filename, "movie");
}

























//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>
#include <iomanip>
#include <limits.h>
#include <stdarg.h>
#include <sstream>

#include <string>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include "unixstuff.h"
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


#include <fstream>
#include <iostream>

#include "mednafen.h"

#include <string.h>
#include <vector>

#include "driver.h"
#include "state.h"
#include "general.h"
#include "video.h"
#include "endian.h"
#include "netplay.h"
#include "movie.h"

#include "endian.h"

#include "drivers/main.h"
//
//struct MovieStruct Movie;
//
//StateMem temporarymoviebuffer;
//
////movie status stuff
//
//static uint32 RerecordCount;
//uint32 FrameCounter;
//
//int readonly = 1; //movie is read only by default
//
//int isMov = 0; //used to create a second set of savestates for nonrecording/nonplayback
////0 = not playing or recording
//
int current = 0;		// > 0 for recording, < 0 for playback
//
/////////////////////
//
//static FILE* slots[10]={0};
//
static int RecentlySavedMovie = -1;
//static int MovieStatus[10];
//static StateMem RewindBuffer;
//
//static uint32 moviedatasize = 0;
//char * tempbuffer;
//
//
////playback related variables
//
//uint8  md5_of_rom_used[16];
//char movieauthor[33];
//char MovieMD5Sum[33];
//uint32 tempmloc;//contains ftell of a playing movie file, allowing resuming a movie from a playback-made savestate possible
//uint32 MoviePlaybackPointer;
//
/////////////////////////
//
//void ResetVariables(void) {
//
//moviedatasize=0;
//tempbuffer = (char*) malloc (sizeof(char)*moviedatasize);
//memset(&temporarymoviebuffer, 0, sizeof(StateMem));
//FrameCounter = 0;
//tempmloc=0;
//MoviePlaybackPointer=0;
//
//}
//
////used for starting recording when a state is loaded in read+write playback
//void SetCurrent(int incurrent) {
//
//	current=CurrentMovie;
//	current++;
//}
//
//void ReadHeader(FILE* headertest) { //TODO actual comparisons
//
//	//check file indicator
//
//	//compare mednafen version  //MEDNAFEN_VERSION_NUMERIC
//	
//	//read32le(version, headertest);
//
//	fseek(headertest, 8, SEEK_SET);
//
//	//compare movie file format version
//
//	uint32 movversion;
//	fseek(headertest, 12, SEEK_SET);
//	movversion = fgetc(headertest);
//
//	//compare MD5 Sums
//
//	fread(md5_of_rom_used, 1, 16, headertest);
//	snprintf(MovieMD5Sum, 16, "%d", md5_of_rom_used);  //for displaying later
//
//	//update rerecords with value from file
//
//	fseek(headertest, 112, SEEK_SET);
//	read32le(&RerecordCount, headertest);
//
//	//read console - only useful for counting frames //probably only useful for the site really
//
//	//read author's name
//
//	fseek(headertest, 121, SEEK_SET);
//	fread(movieauthor, sizeof(char), 32, headertest);
//
//	//finished
//}
//
//void AddRerecordCount(void) { //increment the record counter
//
//	//only if we are in record mode
//	if(current > 0)
//		RerecordCount++;
//
//}
//
//void WriteHeader(FILE* headertest) {
//
//	//FILE* headertest;
//
//	//smem_seek(&temporarymoviebuffer, 0, SEEK_SET);
//	//headertest=fopen("headertest.txt","wb");
//	//tempbuffertest3=fopen(MDFN_MakeFName(MDFNMKF_MOVIE,CurrentMovie,0).c_str(),"wb3");
//
//	fseek(headertest, 0, SEEK_SET);
//
//	//file indicator
//	//MDFNMOVI
//
//	static char MDFNMOVI[9] = "MDFNMOVI";
//
//	//snprintf(writeauthor, 32, "%s", author.c_str());
//
//	//strlen so that we don't write a bunch of junk to the file
//
//	fwrite(MDFNMOVI, sizeof(char), strlen(MDFNMOVI), headertest);
//
//	//write mednafen version
//
//	write32le(MEDNAFEN_VERSION_NUMERIC, headertest);
//
//	//write movie format version
//
//	uint32 MovieVersion = 1;
//
//	write32le(MovieVersion, headertest);
//
//	//write MD5, Filename of the rom
//	//GetMD5AndFilename(headertest);  //up to 64 chars of filename
//
//	snprintf(MovMD5Sum, 16, "%s", MDFNGameInfo->MD5);
//
//	fwrite(MovMD5Sum, sizeof(char), 32, headertest);
//
//	//Filename
//
//	GetMovFileBase(headertest);
//
//	//Rerecords
//	write32le(RerecordCount, headertest);
//
//
//	//console
//
//	static char MovConsole[6];
//
//	snprintf(MovConsole, 6, "%s", CurGame->shortname);
//
//	fwrite(MovConsole, sizeof(char), 5, headertest);
//
//
//	//author's name
//
//	std::string author = MDFN_GetSettingS("author");
//
//	char writeauthor[32];
//
//	snprintf(writeauthor, 32, "%s", author.c_str());
//
//	//strlen so that we don't write a bunch of junk to the file
//
//	fwrite(writeauthor, sizeof(char), strlen(writeauthor), headertest);
//
//	//some padding
//
//
//	int j;
//
//	for (j = 0; j < 103; j++) {
//
//		fputc(0, headertest);
//
//	}
//}
//
void MovClearAllSRAM(void) {

	//TODO ZERO
  memset(SaveRAM, 0x00, 2048);
  memcpy(SaveRAM, "HUBM\x00\xa0\x10\x80", 8);  

}
//
//int MovieFrameCount;  //total number of frames in a movie that is being played back
//
////this is for the sake of properly counting frames
//
////pce defaults
//int NumberOfPorts = 5;
//uint32 PortDataCacheLength = 2;
//
//void SetNumberOfPorts(void) {
//
//	if (strcmp (CurGame->shortname, "lynx") == 0 || strcmp (CurGame->shortname, "wswan") == 0)
//		NumberOfPorts = 1;
//	
//	if (strcmp (CurGame->shortname, "pcfx") == 0 ) 
//		NumberOfPorts = 2;
//	
//	if (strcmp (CurGame->shortname, "pce")== 0 ) 
//		NumberOfPorts = 5;
//	if (strcmp (CurGame->shortname, "sms")== 0 ) {
//		NumberOfPorts = 2;
//		PortDataCacheLength=1;
//	}
//	if (strcmp (CurGame->shortname, "ngp")== 0 ) {
//		NumberOfPorts = 1;
//		PortDataCacheLength=1;
//	}
//	//not really correct, the nes has 5 ports with the last one as length 0 apparently...
//	if (strcmp (CurGame->shortname, "nes")== 0 ) {
//		NumberOfPorts = 4;
//		PortDataCacheLength=1;
//	}
//}
//
//////////////////////////
//
//
//
//void setMoviePlaybackPointer(uint32 value) {
//
//	MoviePlaybackPointer = value;
//
//}
//
//FILE* getSlots() {
//
//	return(slots[-1 - current]);
//
//}
//
//
/////////////////////////////////////////////
////seek to a particular frame in the movie//
/////////////////////////////////////////////
//
//
//void MDFNMOV_Seek(FILE* fp)
//{
//
//	fseek (fp , MoviePlaybackPointer, SEEK_SET );
//}
//
///////////////////////////////////////////
////count the number of frames in a movie//
///////////////////////////////////////////
//
//void MDFNMOV_Count(FILE* fp)
//{
//	MovieFrameCount = 0;
//
//	int t = 0;
//
//	int moviesize1 = 0;
//
//	SetNumberOfPorts();
//	//get the size of the movie
//
//	fseek(fp, 0, SEEK_END);
//	moviesize1=ftell (fp);
//	rewind(fp);
//
//	//each port increases the size of the movie
//	MovieFrameCount = (moviesize1-256) / ((NumberOfPorts*2) + 1); //MovieFrameCount / NumberOfPorts;
//
////	free(junkbuffer);
//}
//
//int getreadonly(void) {
//
//	return(Movie.readonly);
//}
//
//int RecordingSwitchToPlayback;
//
void setreadonly(void) { //it's a toggle

	ToggleReadOnly();

	//if(movie_readonly = false;
/*
	if(!(current > 0)) { //special case if we are recording

		if(Movie.readonly == 1) {
	//		Movie.readonly=0;

			//readonly = 0; // read+write
			MDFN_DispMessage((UTF8 *)_("Read+Write"));
		}

		else {

		//	Movie.readonly=1;
			//readonly = 1;// read onlymovie.cpp:124: error: at this point in file
			MDFN_DispMessage((UTF8 *)_("Read Only"));
		}
	}
	else {
		MDFN_DispMessage((UTF8 *)_("Recording continuing - Load a state to play read-only"));
	//	RecordingSwitchToPlayback=1;
	}*/
}
//
////allows people to start in either readonly or read+write mode by the command line
//void setreadonlycli(int value) {
//
////	readonly = value;
//	Movie.readonly=value;
//
//}
//
////gets the total number of frames to the hud
int DisplayTotalFrames(void) {

	return(1);//MovieFrameCount);
	}
//	MOVIEMODE_RECORD = 2,
//	MOVIEMODE_PLAY = 4,
////display plackback/recording indicator
////also for indicating whether saveram should be cleared
int MovInd(void) 
{
	if(MovieIsActive()) 
	{
		if(movieMode == MOVIEMODE_RECORD) //recording
			return(666);	
		
		else  //playback
			return(333);
	}
	else  
		return(111); //not recording or playback 
}
//
//
////we want a separate set of savestates for nonrecording/nonplayback so movies don't accidentally get ruined
////this is done by increasing the savestate number so that the normal ones won't be overwritten
int retisMov(void) {

	if(MovieIsActive())
		return 10;
	else
		return(NULL);


//	if(isMov == 0) 
//		return(42);  //this could be any number large enough to make the savestates not conflict

//	else 
	//	return(NULL);
}
//
////framecounter functions
//
//void incFrameCounter(void) {
//
//	FrameCounter++;
//}
//
uint32 retFrameCounter(void) {

	return(currFrameCounter);

}
//
uint32 setFrameCounter(uint32 value) {
currFrameCounter=value;
return 1;
}
//	FrameCounter = value;
//	//TODO TODO TODO - zero didnt know what this should do
//	return 0;
//	//TODO TODO TODO - zero didnt know what this should do
//}
//
////used for conditionals in state.cpp depending on whether we are playing back or recording
//int checkcurrent(void) {
//
//	return(current);
//
//}
//
//
//
////used for truncating the movie file when a state is saved during playback
//uint32 getmloc(void) {
//
//	return(tempmloc);
//
//}
//
//
////used in mednafen.cpp for QSimpleCommand and preventing rewinding 
////returns 1 if mdfn is playing back
bool MDFNMOV_IsPlaying(void)
{
	if(current < 0) return(1);
	else return(0);
}
//
////used in netplay and rewinding code
////returns 1 if mdfn is recording
bool MDFNMOV_IsRecording(void)
{
	if(current > 0) return(1);
	else return(0);
}
//
//int firstopen = 1;
//
////when a recording is stopped, write the movie file to disk
//static void StopRecording(void)
//{
//
//	// MDFNMOV_RecordState(); //saves the ending state into the movie file with playback indicator 0x80 - we don't want this to happen
//	if(MDFN_StateEvilIsRunning())  //state rewinding
//	{
//		MDFN_StateEvilFlushMovieLove();
//	}
//	// fclose(slots[current-1]);
//	MovieStatus[current - 1] = 1;
//	RecentlySavedMovie = current - 1;
//	current=0; // we are not recording
//	MDFN_DispMessage((UTF8 *)_("Movie recording stopped."));
//
//	if(RewindBuffer.data)  //get rid of rewind data
//	{
//		//puts("Oops");
//		free(RewindBuffer.data);
//		RewindBuffer.data = NULL;
//	}
//
//	WriteHeader(Movie.fp);
//
//	fclose(Movie.fp);
//
//	isMov = 0;  //change the hud indicator to stopped
//	Movie.status=stopped;
//
//}
//
void MDFNI_SaveMovie(char *fname, uint32 *fb, MDFN_Rect *LineWidths)
{

	if(movie_readonly == true) {
		MDFN_DispMessage((UTF8 *)_("Can't record. Toggle read only first"));
		return;
	}

	if(movieMode == MOVIEMODE_PLAY){	// Can't save a movie during playback.
		MDFN_DispMessage((UTF8 *)_("Can't save a movie during playback."));
		return;
	}

	if(movieMode == MOVIEMODE_RECORD){	//Stop saving if we are recording. 
		FCEUI_StopMovie();
		return;
	}

	std::wstring author = L"blal";

	if(!strcmp(MDFN_GetSettingS("mov").c_str(), "mov PATH NOT SET") == 0)
		FCEUI_SaveMovie(MDFN_GetSettingS("mov").c_str(), author);

		//snprintf(fname, 4096, "%s", MDFN_GetSettingS("mov").c_str());

	
	
	else
		MDFN_DispMessage((UTF8 *)_("Set a movie path."));
}

		//if there's a setting, use the designated movie
	//	if(Movie.fp=fopen(MDFN_GetSettingS("mov").c_str(),"r+w")) {
			
	//	}
//	}

	
//}
//}
//
//	if(Movie.readonly == 1) {
//		MDFN_DispMessage((UTF8 *)_("Can't record. Toggle read only first"));
//		return;
//	}
//	//FILE* fp;
//
//	//movies start at frame zero
//
//	if(current < 0)	// Can't save a movie during playback.
//		return;
//
//	if(current > 0)	//Stop saving if we are recording. 
//	{
//		StopRecording();
//		return;   memset(&RewindBuffer, 0, sizeof(StateMem));
//		RewindBuffer.initial_malloc = 16;
//	}
//
//	FrameCounter = 0;
//	ResetlagCounter();
//	SetNumberOfPorts();
//
//	//memset(&temporarymoviebuffer, 0, sizeof(StateMem));
//
//	memset(&RewindBuffer, 0, sizeof(StateMem));  // init
//	RewindBuffer.initial_malloc = 16;
//
//	current=CurrentMovie;
//
//    MakeMovieFilename("w+b");
//
//	// if(!fp) return;
//
//	// MDFNSS_SaveFP(fp, fb, LineWidths);
//
//	// fseek(fp, 0, SEEK_END);
//
//
//	// fflush(fp, Z_SYNC_FLUSH); // Flush output so that previews will still work right while
//	// the movie is being recorded.  Purely cosmetic. :)
//
//	// slots[current] = fp;
//	current++;  //Recording
//
//
//
//	//start from clean sram
//
//	MovClearAllSRAM();
//
//    fseek(Movie.fp, 256, SEEK_SET);//the header will be written later
//
//	//this actually gets recorded into the movie file
//
//	MDFNI_Power(); //right now, movies always start from power on
//
//	Movie.readonly = 0; //we are Read+Write
//
//	isMov = 1;// use movie specific savestates
//	Movie.status=recording;
//
//	DoFrameAdvance();
//
//	MDFN_DispMessage((UTF8 *)_("Movie recording started."));
//}
//
//char filename[4096];
//
//void MakeMovieFilename(const char* arg) {
//
//	//record should be w+b
//	//play should be r+b
//
//	if(!strcmp(MDFN_GetSettingS("mov").c_str(), "mov PATH NOT SET") == 0){// && firstopen == 1) {
//
//		//if there's a setting, use the designated movie
//		if(Movie.fp=fopen(MDFN_GetSettingS("mov").c_str(),arg)) {
//			snprintf(filename, 4096, "%s", MDFN_GetSettingS("mov").c_str());
//		}
//		//firstopen = 0;  //stuff breaks without this
//	}
//	else
//	{
//		//we do a default movie name
//		Movie.fp=fopen(MDFN_MakeFName(MDFNMKF_MOVIE,CurrentMovie + 666,0).c_str(),arg);
//		snprintf(filename, 4096, "%s", MDFN_MakeFName(MDFNMKF_MOVIE,CurrentMovie + 666,0).c_str());
//	}
//
//	Movie.filename=filename;
//
//}
//
//static void StopPlayback(void)
//{
//	if(RewindBuffer.data)
//	{
//		RewindBuffer.data = NULL;
//	}
//
//	fclose(Movie.fp);
//	Movie.status=stopped;
//	//fclose(slots[-1 - current]);
//	current=0;
//
//	isMov = 0;
//	ResetlagCounter();
//
//	//free(temporarymoviebuffer.data);
//
//	void ResetVariables(void);
//
//	if(!MDFN_GetSettingB("mmm"))
//	MDFN_DispMessage((UTF8 *)_("Movie playback stopped."));
//}
//
//
////used to stop playback and recording when a game is closed, and stop playback during netplay
void MDFNMOV_Stop(void)
{
	FCEUI_StopMovie();
//	if(current < 0) StopPlayback();
//	if(current > 0) StopRecording();
}
//

void MDFNI_LoadMovie(char *fname)
{
	if(movieMode == MOVIEMODE_PLAY) {
		FCEUI_StopMovie();
		return;
	}
	if(!strcmp(MDFN_GetSettingS("mov").c_str(), "mov PATH NOT SET") == 0)
		FCEUI_LoadMovie(MDFN_GetSettingS("mov").c_str(), 1, 0, 0);
	else
		MDFN_DispMessage((UTF8*)_("Specify a movie."));


		//if there's a setting, play from the designated movie
		//if(Movie.fp=fopen(MDFN_GetSettingS("mov").c_str(),"r+b")) {
}


//}
/*
void MDFNI_SaveMovie(char *fname)
{
	if(fname)
FCEUI_SaveMovie(fname, NULL);
	else
		MDFN_DispMessage((UTF8*)_("Specify a movie."));

}*/
////	memset(&temporarymoviebuffer, 0, sizeof(StateMem));
//
//	//free(temporarymoviebuffer.data);
//
//	ResetlagCounter();
////	FILE* fp;
//	//puts("KAO");
//
//	if(current > 0)        // Can't interrupt recording.
//		return;
//
//	if(MDFNnetplay)	// Playback is UNPOSSIBLE during netplay. 
//	{
//		MDFN_DispMessage((UTF8*)_("Can't play movies during netplay."));
//		return;
//	}
//
//	if(current < 0)        // We stop playback when loading a movie. 
//	{
//		StopPlayback();
//		return;
//	}
//
//	MakeMovieFilename("r+b") ;
///*
//	//make sure the setting isn't the default
//	if(!strcmp(MDFN_GetSettingS("mov").c_str(), "mov PATH NOT SET") == 0) {
//
//		//if there's a setting, play from the designated movie
//		if(Movie.fp=fopen(MDFN_GetSettingS("mov").c_str(),"r+b")) {
//		}
//	}
//	else
//	{
//		//do a default movie filename
//		Movie.fp=fopen(MDFN_MakeFName(MDFNMKF_MOVIE,CurrentMovie + 666, 0).c_str(),"r+b");
//		//fp=fopen("stoprecordingsmemmovie.txt","rb");
//	}
//*/
//	if(!Movie.fp) return;
//
//	//count the number of frames in the movie
//	MovieFrameCount = 0;
//
//	MDFNMOV_Count(Movie.fp);
//
//	//currently commented out because I've disabled savestates in movie files
//
//	// if(!MDFNSS_LoadFP(fp)) 
//	// {
//	//  MDFN_DispMessage((UTF8 *)_("Error loading state portion of the movie."));
//	//  return;
//	// }
//
//	//first do the header
//
//	ReadHeader(Movie.fp);
//
//	//load the movie file into a buffer
//
//	///get the size
//
//	fseek(Movie.fp, 0, SEEK_END);
//	Movie.size=ftell (Movie.fp);
//	//moviedatasize = moviedatasize - 256; //subtract length of header
//	fseek(Movie.fp, 256, SEEK_SET);  //seek past header
//
//	//copy it
//
//	/////////////////////////
//
//	//movies start at frame zero
//
//	FrameCounter = 0;
//	SetNumberOfPorts(); //so we can load a state and continue playback correctly
//	current = CurrentMovie;
//	//slots[current] = Movie.fp;
//	current = -1 - current;
//	MovieStatus[CurrentMovie] = 1;
//	Movie.status=playback;
//
//
//	MovClearAllSRAM(); //start from clean sram
//	MDFNI_Power(); /////movies always play back from poweron
//	isMov = 1;  //use movie specfic savestates
//	Movie.readonly = 1;  //we always start read only so that the user can toggle it later
//
//	if(!MDFN_GetSettingB("mmm"))
//	MDFN_DispMessage((UTF8*)_("Read-Only Playback - Frames: %d Re-records: %d Author: %s MD5: %s"), MovieFrameCount, RerecordCount, movieauthor, MovieMD5Sum);
//}
//
//
////takes care of either playing back the controller movie data, or recording it
//
////PDCdata points to PortDataCache, getting the commands from this function out to the emulator, or doing the opposite
//
//// PortDataCache
//// PortDataLenCache
//
////DPCdata and PDClen were originally donutdata and donutlen
//
//int tempcount = 0;
//int AddCommand=0;
//int CommandAdded=0;
//
//void SetCommandAdded(void) {
//
//	CommandAdded=0;
//}
//
//void MDFNMOV_AddJoy(void *PDCdata, uint32 PDClen)
//{
////	FILE* fp;
//
//	int t;
//
//	if(!current) return;	/* Not playback nor recording. */
//	if(current < 0)	/* Playback */
//	{
//
//		//fp = slots[-1 - current];
//
//		//once per frame
//		if(CommandAdded==0) {
//		t = fgetc(Movie.fp);
//		CommandAdded=1;
//		if(t>0)
//			MDFN_DoSimpleCommand(t);
//		}
//		//multiple times per frame
//		if(fread(PDCdata, 1, PDClen, Movie.fp) != PDClen){
//			StopPlayback();
//			return;
//		}
//		//this allows you to save a state during playback and then resume a movie from it
//		//tempmloc = ftell(fp);
//	}
//
//	else			/* Recording */
//	{
//		if(MDFN_StateEvilIsRunning())
//		{
//			smem_putc(&RewindBuffer, 0);
//			smem_write(&RewindBuffer, PDCdata, PDClen);
//		}
//		else
//		{
//			// fp = slots[current - 1];
//			//once per frame
//           if(CommandAdded==0) {
//			fputc(AddCommand, Movie.fp);
//			AddCommand=0;//set the command back to nothing
//			CommandAdded=1;
//		   }
//		   //multiple per frame
//		   fwrite(PDCdata, 1, PDClen, Movie.fp);
//		}
//	}
//}
//
////adds non-controller data commmands like reset and poweron to a movie
void MDFNMOV_AddCommand(int cmd)
{
//	_currCommand |= cmd;

//	if(current <= 0) return;	/* Return if not recording a movie */
//
//	if(MDFN_StateEvilIsRunning())
//		smem_putc(&RewindBuffer, 0);
//	else
//		//fputc(cmd, slots[current - 1]);
//		//smem_putc(&temporarymoviebuffer, cmd);
//		AddCommand=cmd;
}
//
////current behavior is saving the state to the movie file so that the state loads during playback
//void MDFNMOV_RecordState(void) 
//{
//	/*
//	FILE* fp = slots[current - 1];
//	StateMem sm;
//
//	memset(&sm, 0, sizeof(StateMem));  // zero Statemem sm
//	MDFNSS_SaveSM(&sm, 0, 0);
//
//	if(MDFN_StateEvilIsRunning())
//	{
//	smem_putc(&RewindBuffer, MDFNNPCMD_LOADSTATE);
//	smem_putc(&RewindBuffer, sm.len & 0xFF);
//	smem_putc(&RewindBuffer, (sm.len >> 8) & 0xFF);
//	smem_putc(&RewindBuffer, (sm.len >> 16) & 0xFF);
//	smem_putc(&RewindBuffer, (sm.len >> 24) & 0xFF);
//	smem_write(&RewindBuffer, sm.data, sm.len);
//	}
//	else
//	{
//	fputc(fp, MDFNNPCMD_LOADSTATE);
//	fputc(fp, sm.len & 0xFF);
//	fputc(fp, (sm.len >> 8) & 0xFF);
//	fputc(fp, (sm.len >> 16) & 0xFF);
//	fputc(fp, (sm.len >> 24) & 0xFF);
//	fwrite(slots[current - 1], sm.data, sm.len);
//	}
//	free(sm.data);
//	*/
//}
//
//void MDFNMOV_ForceRecord(StateMem *sm) //used during rewinding
//{
//	printf("Farced: %d\n", sm->len);
//	// fwrite(slots[current - 1], sm->data, sm->len);
//}
//
//StateMem MDFNMOV_GrabRewindJoy(void)
//{
//	StateMem ret = RewindBuffer;
//	memset(&RewindBuffer, 0, sizeof(StateMem));
//	RewindBuffer.initial_malloc = 16;
//	return(ret);
//}
//
//void MDFNMOV_CheckMovies(void)
//{
//	time_t last_time = 0;
//
//	for(int ssel = 0; ssel < 10; ssel++)
//	{
//		struct stat stat_buf;
//
//		MovieStatus[ssel] = 0;
//		if(stat(MDFN_MakeFName(MDFNMKF_MOVIE, ssel, 0).c_str(), &stat_buf) == 0)
//		{
//			MovieStatus[ssel] = 1;
//			if(stat_buf.st_mtime > last_time)
//			{
//				RecentlySavedMovie = ssel;
//				last_time = stat_buf.st_mtime;
//			}
//		}
//	}
//	CurrentMovie = 0;
//}
//
void MDFNI_SelectMovie(int w)
{
	FILE* fp;
	uint32 MovieShow = 0;
	uint32 *MovieShowPB = NULL;
	uint32 MovieShowPBWidth;
	uint32 MovieShowPBHeight;

	if(w == -1) { MovieShow = 0; return; }
	MDFNI_SelectState(-1);

	CurrentMovie=w;
	MovieShow = MDFND_GetTime() + 2000;;

	fp = fopen(MDFN_MakeFName(MDFNMKF_MOVIE,CurrentMovie,NULL).c_str(), "rb");

	if(fp)
	{
		uint8 header[32];

		//this should be fixed at some point
		//int gzread  (gzFile file, voidp buf, unsigned int len);
		fread(header, 1, 32, fp);
		uint32 width = MDFN_de32lsb(header + 24);
		uint32 height = MDFN_de32lsb(header + 28);

		if(width > 512) width = 512;
		if(height > 512) height = 512;

		{
			uint8 *previewbuffer = (uint8*)alloca(3 * width * height);
			uint8 *rptr = previewbuffer;

			fread(previewbuffer, 1, 3 * width * height, fp);

			if(MovieShowPB)
			{
				free(MovieShowPB);
				MovieShowPB = NULL;
			}

			MovieShowPB = (uint32 *)malloc(4 * width * height);
			MovieShowPBWidth = width;
			MovieShowPBHeight = height;

			for(unsigned int y=0; y < height; y++)
				for(unsigned int x=0; x < width; x++)
				{
					MovieShowPB[x + y * width] = MK_COLORA(rptr[0],rptr[1],rptr[2], 0xFF);
					rptr+=3;
				}
				fclose(fp);
		}
	}
	else
	{
		if(MovieShowPB)
		{
			free(MovieShowPB);
			MovieShowPB = NULL;
		}
		MovieShowPBWidth = MDFNGameInfo->ss_preview_width;
		MovieShowPBHeight = MDFNGameInfo->height;
	}

	StateStatusStruct *status = (StateStatusStruct*)calloc(1, sizeof(StateStatusStruct));

//	memcpy(status->status, MovieStatus, 10 * sizeof(int));
	status->current = CurrentMovie;
	status->current_movie = current;
	status->recently_saved = RecentlySavedMovie;
	status->gfx = MovieShowPB;
	status->w = MovieShowPBWidth;
	status->h = MovieShowPBHeight;
	status->pitch = MovieShowPBWidth;

	MDFND_SetMovieStatus(status);
}
//
//struct MovieBufferStruct truncatebuffer;
//
//void TruncateMovie(struct MovieStruct mov) {
//	int length;
//
//	//when we resume recording, shorten the mov so that there isn't 
//	//potential garbage data at the end
//
//	fseek(mov.fp,0,SEEK_SET);
//	truncatebuffer=ReadMovieIntoABuffer(mov.fp);
//	fclose(mov.fp);
//	
//	//clear the file and write it again
//	mov.fp=fopen(mov.filename,"wb");
//	//fwrite(truncatebuffer.data,256,1,mov.fp);//header
//	length=(mov.framelength*FrameCounter)+256;
//	fwrite(truncatebuffer.data,1,length,mov.fp);
//	fclose(mov.fp);
//
//	mov.fp=fopen(mov.filename,"r+b");
//}
//
//void MovieLoadState(void) {
//
//	Movie.headersize=256;
//
//	
//	Movie.framelength=PortDataCacheLength*NumberOfPorts;
//	Movie.framelength+=1;//the control bit
//
//	if (Movie.readonly == 1 && Movie.status == playback)  {
//		//Movie.Status = Playback;
//		fseek (Movie.fp,Movie.headersize+FrameCounter * Movie.framelength,SEEK_SET);
//	}
//
//	if(Movie.status == recording) {
//		fseek (Movie.fp,Movie.headersize+FrameCounter * Movie.framelength,SEEK_SET);
//	}
//
//	if(Movie.status == playback && Movie.readonly == 0) {
//		Movie.status = recording;
//		TruncateMovie(Movie);
//		fseek (Movie.fp,Movie.headersize+FrameCounter * Movie.framelength,SEEK_SET);
//		SetCurrent(1);
//		//RecordingFileOpened=1;
//		//strcpy(MovieStatus, "Recording Resumed");
//		
//
//	}
//}
//
//void ReplaceMovie(FILE* fp) {
//
//	struct MovieBufferStruct tempbuffer;
//
//	//overwrite the main movie on disk if we are recording or read+write playback
//	if(Movie.status == recording || (Movie.status == playback && Movie.readonly == 0)) {
//
//		//figure out the size of the movie
//		fseek(fp, 0, SEEK_END);
//        tempbuffer.size=ftell (fp);
//	    rewind(fp);
//
//		//fread(&tempbuffer.size, 4, 1, fp);//size
//		tempbuffer.data = (char*) malloc (sizeof(char)*tempbuffer.size);
//		fread(tempbuffer.data, 1, tempbuffer.size, fp);//movie
//		//fseek(fp, fpos, SEEK_SET);//reset savestate position
//
//		rewind(Movie.fp);
//		fwrite(tempbuffer.data, 1, tempbuffer.size, Movie.fp);
//	}
//}
//
//struct MovieBufferStruct ReadMovieIntoABuffer(FILE* fp) {
//
//	int fpos;
//	struct MovieBufferStruct tempbuffer;
//
//	fpos = ftell(fp);//save current pos
//
//	fseek (fp,0,SEEK_END);
//	tempbuffer.size=ftell(fp);  //get size
//	rewind(fp);
//
//	tempbuffer.data = (char*) malloc (sizeof(char)*tempbuffer.size);
//	fread (tempbuffer.data, 1, tempbuffer.size, fp);
//
//	fseek(fp, fpos, SEEK_SET); //reset back to correct pos
//	return(tempbuffer);
//}
//
//void CopyMovie(FILE* fp) {
//
//	struct MovieBufferStruct tempbuffer;
//
//	if(Movie.status == recording || Movie.status == playback) {
//		tempbuffer=ReadMovieIntoABuffer(Movie.fp);//read in the main movie
//
//		//fwrite(&tempbuffer.size, 4, 1, fp);
//		fwrite(tempbuffer.data, tempbuffer.size, 1, fp);//and copy it to destination
//	}
//}
//
//using namespace std;
//
//bool CheckFileExists(const char* filename)
//{
//	//This function simply checks to see if the given filename exists
//	std::string checkFilename; 
//
//    if (FILE * file = fopen(filename, "r")) //I'm sure, you meant for READING =)
//    {
//        fclose(file);
//        return true;
//    }
//    return false;
//}
////const char * backupfilename;
//
////char * backupfilename;
//
//void FCEUI_MakeBackupMovie(bool dispMessage)
//{
//	FILE* backupfile;
//	
//
//	//This function generates backup movie files
//	std::string currentFn;					//Current movie fillename
//	std::string backupFn;					//Target backup filename
//	std::string tempFn;						//temp used in back filename creation
//	stringstream stream;
//	int x;								//Temp variable for string manip
//	bool exist = false;					//Used to test if filename exists
//	bool overflow = false;				//Used for special situation when backup numbering exceeds limit
//
//	currentFn = Movie.filename;		//Get current moviefilename
//	backupFn = Movie.filename;		//Make backup filename the same as current moviefilename
//	x = backupFn.find_last_of(".");		 //Find file extension
//	backupFn = backupFn.substr(0,x);	//Remove extension
//	tempFn = backupFn;					//Store the filename at this point
//	for (unsigned int backNum=0;backNum<999;backNum++) //999 = arbituary limit to backup files
//	{
//		stream.str("");					 //Clear stream
//		if (backNum > 99)
//			stream << "-" << backNum;	 //assign backNum to stream
//		else if (backNum <=99 && backNum >= 10)
//			stream << "-0";				//Make it 010, etc if two digits
//		else
//			stream << "-00" << backNum;	 //Make it 001, etc if single digit
//		backupFn.append(stream.str());	 //add number to bak filename
//		backupFn.append(".bak");		 //add extension
//
//		exist = CheckFileExists(backupFn.c_str());	//Check if file exists
//		
//		if (!exist) 
//			break;						//Yeah yeah, I should use a do loop or something
//		else
//		{
//			backupFn = tempFn;			//Before we loop again, reset the filename
//			
//			if (backNum == 999)			//If 999 exists, we have overflowed, let's handle that
//			{
//				backupFn.append("-001.bak"); //We are going to simply overwrite 001.bak
//				overflow = true;		//Flag that we have exceeded limit
//				break;					//Just in case
//			}
//		}
//	}
//
////	MovieData md = currMovieData;								//Get current movie data
//
//	//snprintf(backupfilename, 1023, "%s", backupFn);
//	//backupfilename = backupFn.c_str ();
//	//backupfilename = strdup(backupFn.c_str());
//
//
//	//std::string str;
//	char * backupfilename = new char[backupFn.size() + 1];
//	std::copy(backupFn.begin(), backupFn.end(), backupfilename);
//	backupfilename[backupFn.size()] = '\0'; // don't forget the terminating 0
//
//
//
//	backupfile=fopen(backupfilename, "wb");	//open/create file
//    CopyMovie(backupfile);
//
//		// don't forget to free the string after finished using it
//	delete[] backupfilename;
//	//md.dump(outf,false);										//dump movie data
////	delete outf;												//clean up, delete file object
//	
//	//TODO, decide if fstream successfully opened the file and print error message if it doesn't
//
////	if (dispMessage)	//If we should inform the user 
////	{
//	//	if (overflow)
//	//		FCEUI_DispMessage("Backup overflow, overwriting %s",backupFn.c_str()); //Inform user of overflow
//	//	else
//	//		FCEUI_DispMessage("%s created",backupFn.c_str()); //Inform user of backup filename
////	}
//}
//
//
//
//
void SetCommandAdded() {}
void setreadonlycli() {}
void incFrameCounter() {}
void SetlagCouter(int set) {};
