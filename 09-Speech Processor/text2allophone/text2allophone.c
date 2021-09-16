// TEXT to SP0256a-AL2 Allophone converter by Brian Chiha
//-------------------------------------------------------
//
// Designed to be used for the Talking Electroinc Computer Speech module add on
//
// Compile and run, then type some text.  Type PP to insert a 200ms pause and EOF 
// to insert 0xFF (End of file for TEC running program).  Control-D to exit.
// Note: Puctuation must be seperated with a space or don't use it at all.
//
// Output is given as Allophones and the SP0256a-AL2 hex equivilant.  If a word
// can't be found '??' is outputed and a '.' represents a word gap.  A pause of 100ms is
// automatically placed between words.
//
// To be used with the TEC Speech Module attached to Port 7.  Port can be changed see below
//
// Options
// -b 		Output Binary File which includes main program code
// -w       Don't include main program in bianary file (used with -b)
// -p xx    Change the output port.  Default is port 7 (used with -b)
// -d       Output allophones as Z80 Assembler DB format for easy code insersion
//
// For binary output to directly load into the TEC. use -b as a command line option.
// Binary files are created with the code to activate the Speech Module and the speech 
// data.  If you just want the allophones to be outputted use -w as well.  A new file is created
// after each Carriage Return.  Binary file are to be loaded at address 0x900 on the TEC
//
// Files that are requried are:
// text2allophone.c   - This Program
// cmudict-0.7b       - CMU dictionary file that associates words to the ARPAbet phoneme set
//                    - see http://www.speech.cs.cmu.edu/cgi-bin/cmudict
// cmu2sp0.symbols    - Convert CMU to SPO256a Allophones

/* sample output from the program
> Welcome to Talking Electronics PP I hope you have a great day ! EOF
TXT> WW EH LL PA3 KK2 AX MM . PA3 TT2 UW2 . PA3 TT2 AO PA3 KK2 IH
     NG . IH LL EH PA3 KK2 PA3 TT2 RR1 AA NN1 IH PA3 KK2 SS . AY .
     HH2 OW PA3 PP . YY2 UW2 . HH2 AE VV . AX . PA2 GG3 RR1 EY PA3
     TT2 . PA2 DD2 EY . EH PA3 KK2 SS PA3 KK2 LL AX MM EY SH AX NN1
     PA3 PP OY NN1 PA3 TT2 .

000> 2E 07 2D 02 29 0F 10 03
008> 02 0D 1F 03 02 0D 17 02
010> 29 0C 2C 03 0C 2D 07 02
018> 29 02 0D 0E 18 0B 0C 02
020> 29 37 03 04 06 03 39 35
028> 02 09 03 19 1F 03 39 1A
030> 23 03 0F 03 01 22 0E 14
038> 02 0D 03 01 21 14 03 07
040> 02 29 37 02 29 2D 0F 10
048> 14 25 0F 0B 02 09 05 0B
050> 02 0D 03 FF

DB 2EH,07H,2DH,02H,29H,0FH,10H,03H
DB 02H,0DH,1FH,03H,02H,0DH,17H,02H
DB 29H,0CH,2CH,03H,0CH,2DH,07H,02H
DB 29H,02H,0DH,0EH,18H,0BH,0CH,02H
DB 29H,37H,03H,04H,06H,03H,39H,35H
DB 02H,09H,03H,19H,1FH,03H,39H,1AH
DB 23H,03H,0FH,03H,01H,22H,0EH,14H
DB 02H,0DH,03H,01H,21H,14H,03H,07H
DB 02H,29H,37H,02H,29H,2DH,0FH,10H
DB 14H,25H,0FH,0BH,02H,09H,05H,0BH
DB 02H,0DH,03H,0FFH

*/

/* For reference, here is the Test Program that is used to run the Speech Module taking in data
   for the SP0256a-AL2 chip.  It is included in the binary output by default

0900    21 0B 09    LD HL,090C      ;Location of allophone data
0903    01 pp ss    LD BC,SIZE + PORT ;Load BC with length of allophone data and output port
0906    ED B3       OTIR            ;Output Contents of HL, to port C, B times 
0908    76          HALT            ;Wait for key input as EOF has reached
0909    18 F5       JR 0900         ;Jump back to start
090B    <start of Allophone data>

Note: SIZE (ss) and PORT (pp) are calculated at runtime based on inputs
*/

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sox.h>

#define SYMBOL_LEN 4
#define MAX_SYMBOLS 84
#define MAX_TREE_HEIGHT 20
#define MAX_KEY_LENGTH 35
#define MAX_INPUT_SIZE 5000
#define WHITESPACE " "
#define PORT 7

#define ODD(x) ((x)/2*2 != (x))

int test_code_z80[11] = {0x21,0x0B,0x09,0x01,0x00,0x00,0xED,0xB3,0x76,0x18,0xF5};

FILE *dictptr=NULL;   //file pointer to dictionary

//data structures
typedef struct symdat {
    char cmu[SYMBOL_LEN];
    char sp0[SYMBOL_LEN];
    unsigned int hex;
} Symdat;

//list structure
typedef struct list_tag {
	char symbol[SYMBOL_LEN];
	struct list_tag *next;
} List_type;

//tree structure
typedef struct node_tag {
    char key[MAX_KEY_LENGTH];
    List_type *allophones;
    struct node_tag *left;
    struct node_tag *right;
} Node_type;

void upper_string(char s[]) {
   int c = 0;
   
   while (s[c] != '\0') {
      if (s[c] >= 'a' && s[c] <= 'z') {
         s[c] = s[c] - 32;
      }
      c++;
   }
}

//Binary Tree functions for dictionary

//Find the highest power of 2 that divides count
int Power2(int count) {
    int level;
    
    for (level = 0; !ODD(count); level++)
        count /= 2;
    return level;
}

//Find the root of the tree
Node_type *FindRoot(Node_type *lastnode[]) {
    int level;
    for (level = MAX_TREE_HEIGHT-1; level > 0 && !lastnode[level]; level--)
        ;  //get highest level that isn't null
    if (level <= 0)
        return NULL;
    else
        return lastnode[level];
}

//Connect free subtrees from lastnode[]
void ConnectSubtrees(Node_type *lastnode[]) {
    Node_type *p;
    int level, templevel;
    
    for (level = MAX_TREE_HEIGHT-1; level > 2 && !lastnode[level]; level--)
        ;
    while (level > 2) {
        if (lastnode[level]->right)
            level--;
        else {
            p = lastnode[level]->left;
            templevel = level - 1;
            do {
                p = p->right;
            } while (p && p == lastnode[--templevel]);
            lastnode[level]->right = lastnode[templevel];
            level = templevel;
        }
    }
}

//Insert node p into the right most node of the tree
void InsertNode(Node_type *p, int count, Node_type *lastnode[]) {
    int level = Power2(count) + 1;
    
    p->right = NULL;
    p->left = lastnode[level - 1];
    lastnode[level] = p;
    if (lastnode[level+1] && !lastnode[level+1]->right)
        lastnode[level+1]->right = p;
}

//Get the next word from dictionary file
Node_type *GetNode(void) {
    Node_type *p=NULL;
    List_type *prev_head=NULL;

    char line[128];
    char *token;
    
    if (!feof(dictptr) && fgets(line, 128, dictptr)) {
        /* remove new line character*/
        char *newline = strchr(line, '\n');
        if (newline)
        	*newline = 0;
        /* get the word */
        token = strtok(line, WHITESPACE);
        p = malloc(sizeof(Node_type));

        strcpy(p->key,token);
        p->left = p->right = NULL;
        p->allophones = NULL;
        /* walk through the allophones and add them to the node */
      	token = strtok(NULL, WHITESPACE);  //will have atleast 1 allophone
		while( token != NULL ) {
      		//create list
      		List_type* sp = malloc(sizeof(List_type));
      		strcpy(sp->symbol,token);
      		sp->next = NULL;
      		//add list to end of symbols
      		if (p->allophones)
   				prev_head->next = sp;
      		else
      			p->allophones = sp;
			prev_head = sp;
            token = strtok(NULL, WHITESPACE);
   		}
    }
    return p;
}


//Build Tree assumes entries are already sorted when inserted
Node_type *BuildCMUTree(char filename[]) {
    Node_type *p;   //current node
    int count = 0;  //number of nodes so far
    int level;      //level for current nodes
    Node_type *lastnode[MAX_TREE_HEIGHT];  //last node of each level
    
    for (level = 0; level < MAX_TREE_HEIGHT; level++)
        lastnode[level] = NULL;

    //open cmu to sp0 file
    if ((dictptr = fopen(filename,"r")) == NULL) {
       fprintf(stderr, "Error! opening %s file\n", filename);
       exit(1);
    }

    while ((p=GetNode()) != NULL)
        InsertNode(p, ++count, lastnode);
    p = FindRoot(lastnode);
    ConnectSubtrees(lastnode);
    return p;
}

//Tree search and return key
Node_type *TreeSearch(Node_type *p, char target[]) {
    while (p && strcmp(target, p->key) != 0)
        if (strcmp(target, p->key) < 0)
            p = p->left;
        else
            p = p->right;
    return p;
}

Symdat **LoadSymbolTable(char filename[]) {
    FILE *fp=NULL;
    Symdat **sd;
    char tmp_cmu[SYMBOL_LEN], tmp_sp0[SYMBOL_LEN];
    unsigned int tmp_hex;
    int sd_index=0;
    sd = malloc(sizeof(Symdat)*MAX_SYMBOLS);
    
    //open cmu to sp0 file
    if ((fp = fopen(filename,"r")) == NULL) {
       fprintf(stderr, "Error! opening %s file\n",filename);
       exit(1);
    }
    //read file and store in sd array
    while(fscanf(fp,"%3s %3s %x",tmp_cmu, tmp_sp0, &tmp_hex)==3) {
        Symdat * newSymdat;
        newSymdat = malloc(sizeof(Symdat));
        for (int i = 0; i < 4; i++) {
            newSymdat->cmu[i] = tmp_cmu[i];
            newSymdat->sp0[i] = tmp_sp0[i];
        }
        newSymdat->hex = tmp_hex;
        sd[sd_index] = newSymdat;
        sd_index++;
    }
    fclose(fp);
    return sd;
}

// Main entry point.  Takes in options args -b and -w
int main(int argc, char *argv[]) {
    FILE *fp=NULL,*fout=NULL;
    Symdat **sd;
    char *token, *sp0;
    Node_type *dictionary, *target;
    List_type *curr_symbol;
    char input_text[MAX_INPUT_SIZE];
    char bin_file_name[14];
    unsigned int input_hex[MAX_INPUT_SIZE];
    int sd_index,hex_index=0;
    int opt;
    int f_binary_file=0;
    int f_without_header=0;
    int f_assembly_output=0;
    int file_count=0;
    int port=PORT;

    //parse options if any
	while((opt = getopt(argc, argv, "p:bwd")) != -1) 
	{ 
		switch(opt) 
		{ 
			case 'b':
				f_binary_file = 1;
				break;
			case 'w':
				f_without_header = 1; 
				break;
            case 'p':
                port=atoi(optarg);
                break;
            case 'd':
                f_assembly_output = 1;
                break;
            default:
                fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                return 1;
		} 
	} 
	
    //load data files
    sd = LoadSymbolTable("cmu2sp0.symbols");
    dictionary = BuildCMUTree("cmudict-0.7b");
    
    //get input text
    printf("Text to SPO256-AL2 converter by Brian Chiha\n");
    printf("-------------------------------------------\n");
    printf("Type in a sentence to convert...(EOF for FF, PP for 200ms Pause, C-d to exit)\n\n> ");
    
    while(fgets(input_text,MAX_INPUT_SIZE,stdin)) {
	    /* remove new line character*/
	    char *newline = strchr(input_text, '\n');
	    *newline = 0;

	    // parse words keyed in and convert to allophones
		printf("TXT> ");
		for (token = strtok(input_text, WHITESPACE); token != NULL; token = strtok(NULL, WHITESPACE))
		{
			upper_string(token);
			target = TreeSearch(dictionary,token);
			if (target) {
				curr_symbol = target->allophones;
	        	while (curr_symbol) {
	        		for (sd_index=0; strcmp(sd[sd_index]->cmu,curr_symbol->symbol) != 0; sd_index++)
	        			;
                    sp0 = sd[sd_index]->sp0;
                    // add pauses before certian allophones
                    if (strcmp(sp0,"BB1") == 0 ||
                        strcmp(sp0,"DD2") == 0 ||
                        strcmp(sp0,"GG3") == 0 ||
                        strcmp(sp0,"JH") == 0)
                    {
                        printf("PA2 ");
                        input_hex[hex_index++] = 0x01; 
                    }
                    if (strcmp(sp0,"CH") == 0 ||
                        strcmp(sp0,"KK2") == 0 ||
                        strcmp(sp0,"PP") == 0 ||
                        strcmp(sp0,"TT2") == 0)
                    {
                        printf("PA3 ");
                        input_hex[hex_index++] = 0x02; 
                    }
                    printf("%s ",sp0);
	        		input_hex[hex_index++] = sd[sd_index]->hex; 
	        		curr_symbol = curr_symbol->next;
	        	}
		        printf(". ");  //end of word
		        input_hex[hex_index++] = 0x03; //word pause gap
			}
			else 
			    if (strcmp(token,"EOF") == 0)
			    	input_hex[hex_index++] = 0x0FF;
			    else if (strcmp(token,"PP") == 0)
			    	input_hex[hex_index++] = 0x04;
			    else
			     printf("?? . "); //no word found
		}

        //open file if binary output selected
        if (f_binary_file) {
            sprintf(bin_file_name,"speech%03d.bin",file_count++);
            fout = fopen(bin_file_name,"wb");
            if (!f_without_header)
            {
                //modify port
                if (port == 0)
                    port = PORT;
                test_code_z80[4] = port;
                //modify size
                test_code_z80[5] = hex_index-1;
                for (int address = 0; address < 11; address++)
                    fprintf(fout,"%c",test_code_z80[address]);
            }
        } 

        printf("\n");
        // display hex data stored in input_hex
		for (int i = 0; i < hex_index ; i++) {
			if ((i % 8) == 0)
				printf("\n%03X> ",i);
			printf("%02X ",input_hex[i]);
			if (f_binary_file)
				fprintf(fout,"%c",input_hex[i]);
		}

        // display hex data as Z80 Assembly DB format
        if (f_assembly_output)
        {
            printf("\n");
            for (int i = 0; i < hex_index ; i++) {
                if ((i % 8) == 0)
                    printf("\nDB ");
                if ((i % 8) == 7 || i == hex_index -1 )
                    if (input_hex[i] == 0xFF)
                        printf("%03XH",input_hex[i]);
                    else
                        printf("%02XH",input_hex[i]);
                else
                    printf("%02XH,",input_hex[i]);
            }
        }

		//reset indexes and close file
		hex_index=0;
		if (fout)
			fclose(fout);
		printf("\n\n> ");
	}
    return 0;
}

