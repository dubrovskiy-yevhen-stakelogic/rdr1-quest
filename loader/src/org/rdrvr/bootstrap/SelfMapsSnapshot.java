package org.rdrvr.bootstrap;

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

final class SelfMapsSnapshot {
    static final int MAX_BYTES=2*1024*1024;
    private SelfMapsSnapshot() { }
    static String readSelf() throws IOException {
        try(InputStream input=new FileInputStream("/proc/self/maps")) {
            return readComplete(input);
        }
    }
    static String readComplete(InputStream input) throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream(16384);
        byte[] block=new byte[8192];
        for(;;) {
            int count=input.read(block);
            if(count==-1)break;
            if(count<=0)throw new IOException("maps read made no progress");
            if(count>MAX_BYTES-bytes.size())throw new IOException("maps exceed bound");
            bytes.write(block,0,count);
        }
        byte[] complete=bytes.toByteArray();
        if(complete.length==0 || complete[complete.length-1]!='\n')
            throw new IOException("maps incomplete");
        String result=StandardCharsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(complete)).toString();
        if(result.indexOf('\0')>=0)throw new IOException("maps contain NUL");
        return result;
    }
}
