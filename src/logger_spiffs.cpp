#include "logger_spiffs.h"

#include <FS.h>
#ifdef ESP32
#include <SPIFFS.h>
#endif

bool LoggerSPIFFS::append(String message, bool timestamp){
  unsigned int total=0;
  
  if(timestamp){
    int now=millis();
    String nowString(now);
    message = nowString + " " + message;
  }
    
  // if strict, calculate the next file size
  // (including the line to be recorded)
  if(strictLimit){  
    total = message.length();
    // +2 because 2 bytes are required at the end of each line in this FS implementation
    total += 2;
  }

  if(message.length()+1>sizeLimitPerChunk){
    if (debugVerbosity>0) Serial.println("[ESP LOGGER] @@@@ FATAL ERROR: This message is too large, it can't be neither stored nor sent due to limitation on chunk size, please change it before continue!!!");
    return false;
  }
  
  // Trick to be sure that an empty file exist, and it's dimension is 0 (BUG in esp32)
  if(!SPIFFS.exists(filePath)){
    File file=SPIFFS.open(filePath,"w");
    if(file){
      file.close();
    }
  }

  File f=SPIFFS.open(filePath,"a");
  if(f){
    total += f.size();
    if (debugVerbosity>1) Serial.println(String("[ESP LOGGER] ") + total + "/" + sizeLimit + "bytes are already occupied");
    if (debugVerbosity>1) Serial.println(String("[ESP LOGGER] Recording message: ___") + message + "___");
    if(total>sizeLimit){
      if(message.length()+2>sizeLimit){
      	if (debugVerbosity>0) Serial.println("[ESP LOGGER] @@@@ FATAL ERROR: This message is too large, it can't be stored nor sent due to limitation on file size, please change it before continue!!!");
      }else{
      	if (debugVerbosity>0) Serial.println("[ESP LOGGER] You have reached the maximum file length, the record can't be stored. Please flush the log.");
      }
      f.close();
      return false;
    }
    f.println(message);
    f.close();
    return true;
  }else{
    if (debugVerbosity>0) Serial.println("[ESP LOGGER] Opening log file error!");
  }
  return false;
}

void LoggerSPIFFS::reset(){
  if (debugVerbosity>1) Serial.println("[ESP LOGGER] Resetting the log file...");
  SPIFFS.remove(filePath);
}

static void saveChunk(File& file, char* buffer, int nBuffer){
  int bufferIndex=0;
  // Check if there is another string to print
  while(bufferIndex<nBuffer && strlen(&buffer[bufferIndex])>0){
    int bytePrinted = file.println(&buffer[bufferIndex]);
    // -2, the '\r' and '\n' are printed automatically in the println
    // +1, the '\0' in the buffer is automatically processed but not printed
    bufferIndex += bytePrinted - 2 + 1;
  }
}

static void saveRemainings(File& destination, File& source){
  while(source.available()>2){
    String s=source.readStringUntil('\n');
    // first param included, second paramenter excluded
    // this should remove '\r'
    s=s.substring(0,s.length()-1);
    destination.println(s);
  }
}

void LoggerSPIFFS::flush(){
  if(debugVerbosity>1) Serial.println("[ESP LOGGER] Flushing the log file...");
  
  if(!SPIFFS.exists(filePath)){
    if (debugVerbosity > 1) Serial.println("[ESP LOGGER] File doesn't exist, nothing to flush..");
    return;
  }
  
  File f=SPIFFS.open(filePath,"r");
  if(f){
    bool successFlush=true;
    String line;
    char* buffer = (char*) malloc(sizeLimitPerChunk);

    
    bool bufferFull = false;
    int chunkCount;
    unsigned int nBuffer;
    for(chunkCount = 0;;chunkCount++){
      if(debugVerbosity > 1) Serial.println(String("[ESP LOGGER] :::::::::::::::::::::::::::") + chunkCount);
      
      // First step: fill the buffer with a chunk of data
      if(debugVerbosity > 1) Serial.println("[ESP LOGGER] :::::::::::::::First step: Chunk loading...");
      
      nBuffer = 0;
      bool doRead = true;
      if(bufferFull){
        // This means that we have a "pending" line, no need to read from file
        doRead = false;  
      }
      bufferFull = false;
      while((f.available() || !doRead) && !bufferFull){
        // '\n' is not included in the returned string, but the last char '\r' is
        if(doRead){
          line=f.readStringUntil('\n');
        }else{
          doRead=true;
        }
        
        // l contains the number of byte required by a line (we have to keep into account the '\0')
        // In this case, +1 isn't needed because the _line_ contains the useless '\r'
        unsigned int len=line.length();
        if(len+nBuffer>sizeLimitPerChunk){
          if(debugVerbosity>1) Serial.println(String("[ESP LOGGER] Chunk buffer is almost full: ") + nBuffer + "/" + sizeLimitPerChunk + "byte, cannot store another message, it's time to send..");
          if(len>sizeLimitPerChunk){
          	if (debugVerbosity>0) Serial.println(String("[ESP LOGGER] @@@@ FATAL ERROR: This message is too large (") + len + "/" + sizeLimitPerChunk + "), it can't be store in the chunk, please increase it's size") ;
          } 
          bufferFull=true;
        }else{
          if(debugVerbosity > 1) Serial.print(String("[ESP LOGGER] ###") + line.c_str() + "###");
          if(debugVerbosity > 1) Serial.println(String(" Line length: ") + line.length() + "");
          // remove the last char, that is '\r'
          line=line.substring(0,line.length()-1);
          strcpy(&buffer[nBuffer], line.c_str());
          
          //len includes '\0'
          nBuffer += len;
        }
        
        if(oneRecordPerChunk) break;
      } // END OF WHILE - FILLING THE CHUNK
      
      if(nBuffer == 0){
        if(debugVerbosity > 1) Serial.println("[ESP LOGGER] No more data to send");
        break;
      }
  
      // Second step: send chunk
      if(debugVerbosity > 1) Serial.println("[ESP LOGGER] :::::::::::::::Second step: Chunk flushing...");
      successFlush=flusher(buffer,nBuffer);
      if(!successFlush) break;

    } // END OF FOR - ITERATING OVER THE CHUNKS

    if(!successFlush){
      if(chunkCount > 0){
        if(debugVerbosity > 1) Serial.println("[ESP LOGGER] Partial unsuccessful sending!");
        // I have to discard the successfully sent log, and save the remainings.
        String tempFilePath = filePath+".temp";
        File tempFile=SPIFFS.open(tempFilePath,"w");
        if(f){
          saveChunk(tempFile,buffer,nBuffer);
          saveRemainings(tempFile, f);
          tempFile.close();
          f.close();
          // Riordino i file
          if(SPIFFS.remove(filePath)){
            if (debugVerbosity>1) Serial.println("[ESP LOGGER] The old file is deleted!");
            if(SPIFFS.rename(tempFilePath, filePath)){
              if (debugVerbosity>1) Serial.println("[ESP LOGGER] The temp file is moved!");
            }else{
              if (debugVerbosity>0) Serial.println("[ESP LOGGER] The temp file wasn't moved");
            }
          }else{
            if (debugVerbosity>0) Serial.println("[ESP LOGGER] The temp file is NOT deleted!");
          }
          
          return;
        }else{
          if (debugVerbosity>0) Serial.println("[ESP LOGGER] Writing temp log file error!");
        }
        
      }else{
        // Nothing was sent, so I can close the file and exit from this function
        if(debugVerbosity > 1) Serial.println("[ESP LOGGER] Unsuccessful sending! Nothing is flushed..");
      }
    }else{
      f.close();
      SPIFFS.remove(filePath);
    }

    // Free the memory buffer
    free(buffer);
  }else{
    if (debugVerbosity>0) Serial.println("[ESP LOGGER] Opening log file error!");
  }
  if(debugVerbosity>1) Serial.println("[ESP LOGGER] End of flushing the log file!");
}

LoggerSPIFFS::LoggerSPIFFS(String file, int debugVerbosity): Logger(file,debugVerbosity){
};

bool LoggerSPIFFS::begin(){
  Serial.print("[ESP LOGGER] Filesystem initialization... ");
  if(!SPIFFS.begin()){
    Serial.print("Error in starting file system, you could try to format it...");
    return false;
  }else{
    Serial.println("Done!");
    return true;
  }    
}

unsigned int LoggerSPIFFS::getActualSize(){
  unsigned int result=0;
  File file;
  // Trick to be sure that an empty file exist, and it's dimension is 0 (BUG in esp32)
  if(!SPIFFS.exists(filePath)){
    file=SPIFFS.open(filePath,"w");
    if(file){
      file.close();
    }else{
      if (debugVerbosity>0) Serial.println("[ESP LOGGER] getActualSize() open log file error!");
    }
  }

  file=SPIFFS.open(filePath, "r");
  if(file){
    result=file.size();
    file.close();
  }
  return result;
}