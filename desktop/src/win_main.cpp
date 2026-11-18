// Windows entry point: the game is a GUI program (no console window); the
// engine's own main() in Code/Sk/Main.cpp does the work. The log goes to
// thug.log beside the game data.
#include <windows.h>
#include <stdlib.h>

int main( int argc, char *argv[] );

int WINAPI WinMain( HINSTANCE, HINSTANCE, LPSTR, int )
{
	return main( __argc, __argv );
}
