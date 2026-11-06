/* Opt-in 20H392 recovery dependency for the existing mobile lsd only.
 * Keep MI's complete records. Never call legacy unregister/registerPlugin:.
 * Runtime acceptance remains pending; this is not a provider visibility claim. */
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef void *Id;
static FILE *diagnostic;
static void log_message(const char *format,...) {
    va_list arguments;
    va_start(arguments,format);
    vfprintf(diagnostic ? diagnostic : stderr,format,arguments);
    va_end(arguments);
    fflush(diagnostic ? diagnostic : stderr);
}
static void error_message(unsigned int n,const char *phase,Id error);
static Id (*klass)(const char *), (*selector)(const char *);
static void *message;
static Id (*method)(Id, Id), (*implementation)(Id);
static Id (*object_class)(Id);
static const char *(*encoding)(Id);
static void *(*pool_push)(void);
static void (*pool_pop)(void *);
static unsigned char *core;
#define M0(o,s) ((Id (*)(Id,Id))message)((o),selector(s))
#define M1(o,s,a) ((Id (*)(Id,Id,Id))message)((o),selector(s),(Id)(a))
#define M2(o,s,a,b) ((Id (*)(Id,Id,Id,Id))message)((o),selector(s),(Id)(a),(Id)(b))
#define CHOOSE(_1,_2,_3,_4,NAME,...) NAME
#define SEND(...) CHOOSE(__VA_ARGS__,M2,M1,M0)(__VA_ARGS__)
static unsigned char truth(Id o, const char *s, Id a) {
    return ((unsigned char (*)(Id,Id,Id))message)(o,selector(s),a);
}
static Id str(const char *s) { return SEND(klass("NSString"),"stringWithUTF8String:",s); }
static Id get(Id o,const char *key) { return SEND(o,"objectForKey:",str(key)); }
static int kind(Id o,const char *name) { return o && truth(o,"isKindOfClass:",klass(name)); }
static int equal(Id a,Id b) { return a && b && truth(a,"isEqual:",b); }
static Id url(const char *p) { return SEND(klass("NSURL"),"fileURLWithPath:",str(p)); }
static void error_message(unsigned int n,const char *phase,Id error) {
    const char *description=error ?
        (const char *)SEND(SEND(error,"description"),"UTF8String") : NULL;
    log_message("SweetRepairPosterMetadata: provider=%u phase=%s error=%s\n",
                n,phase,description ? description : "unavailable");
}

static const unsigned char core_uuid[16] = {
    0x8d,0x42,0x10,0x3d,0xbd,0x59,0x30,0x8d,0x9d,0x2c,0x20,0x82,0xb3,0x97,0x38,0xe7 };
static const unsigned char lsd_uuid[16] = {
    0xcf,0xa9,0xbb,0x6b,0x7d,0x4b,0x33,0x97,0x94,0xf6,0x59,0x5d,0x6f,0x1e,0xdd,0xaa };
static unsigned char *image(const char *path,const unsigned char uuid[16]) {
    for(uint32_t i=0;i<_dyld_image_count();i++) {
        const char *name=_dyld_get_image_name(i);
        if(!name || strcmp(name,path)) continue;
        const struct mach_header_64 *h=(const void *)_dyld_get_image_header(i);
        if(!h || h->magic!=MH_MAGIC_64 || h->cputype!=CPU_TYPE_ARM64 ||
           h->ncmds>4096 || h->sizeofcmds>16*1024*1024) return NULL;
        const unsigned char *at=(const void *)(h+1);
        size_t remaining=h->sizeofcmds;
        for(uint32_t n=0;n<h->ncmds;n++) {
            if(remaining<8) return NULL;
            const struct load_command *c=(const void *)at;
            if(c->cmdsize<8 || c->cmdsize>remaining) return NULL;
            if(c->cmd==LC_UUID) return c->cmdsize==24 &&
                !memcmp(((const struct uuid_command *)c)->uuid,uuid,16) ? (void *)h : NULL;
            remaining-=c->cmdsize; at+=c->cmdsize;
        }
    }
    return NULL;
}
static int pinned_method(Id cls,const char *name,uintptr_t offset,const char *types) {
    Id m=method(cls,selector(name));
    return m && implementation(m)==core+offset && (!types || !strcmp(encoding(m),types));
}
static int preimage(uintptr_t offset,unsigned int bytes,const char *hex) {
    unsigned char digest[32]; char actual[65];
    unsigned char *(*sha)(const void *,unsigned int,unsigned char *)=dlsym(RTLD_DEFAULT,"CC_SHA256");
    if(!sha) {
        void *crypto=dlopen("/usr/lib/system/libcommonCrypto.dylib",RTLD_NOW|RTLD_LOCAL);
        if(crypto) sha=dlsym(crypto,"CC_SHA256");
    }
    if(!sha || !sha(core+offset,bytes,digest)) return 0;
    for(unsigned int n=0;n<32;n++) snprintf(actual+n*2,3,"%02x",digest[n]);
    return !strcmp(actual,hex);
}
struct provider { const char *parent,*path,*identifier; };
static const struct provider providers[2]={
    {"/Applications/PaperBoard.app","/Applications/PaperBoard.app/PlugIns/LegacyPoster.appex","com.apple.PaperBoard.LegacyPoster"},
    {"/System/Library/PrivateFrameworks/WallpaperKit.framework","/System/Library/PrivateFrameworks/WallpaperKit.framework/PlugIns/CollectionsPoster.appex","com.apple.WallpaperKit.CollectionsPoster"}
};

/* Returns -1 when verification is unavailable, 0 for a readable mismatch,
 * and 1 only for a valid record with the full genuine MI entitlement dictionary.
 * Unknown concrete dispatch fails closed before either registration. */
static int record_matches(unsigned int n,Id expected) {
    Id cls=klass("LSApplicationExtensionRecord"), error=NULL;
    Id record=((Id (*)(Id,Id,Id,unsigned char,Id *))message)(
        SEND(cls,"alloc"),selector("initWithURL:requireValid:error:"),url(providers[n].path),1,&error);
    if(!record) { error_message(n,"valid LS record initialization",error); return -1; }
    int result=-1;
    if(!pinned_method(object_class(record),"entitlements",0x65b8,"@16@0:8")) goto done;
    if(!equal(SEND(record,"bundleIdentifier"),str(providers[n].identifier))) goto done;
    Id entitlements=SEND(record,"entitlements");
    if(!entitlements || !pinned_method(object_class(entitlements),
          "_expensiveDictionaryRepresentation",0x11d6c,"@16@0:8")) goto done;
    /* propertyList itself is a direct, unregistered native method. The proven
     * registered _expensiveDictionaryRepresentation thunk invokes it; never
     * try to resolve or message the direct selector. */
    if(!pinned_method(object_class(entitlements),"_getPropertyList:",0x6120,"B24@0:8^@16") &&
       !pinned_method(object_class(entitlements),"_getPropertyList:",0x61b8,"B24@0:8^@16")) goto done;
    Id dictionary=SEND(entitlements,"_expensiveDictionaryRepresentation");
    if(kind(dictionary,"NSDictionary")) result=equal(dictionary,expected);
done:
    SEND(record,"release");
    return result;
}

static void repair(void) {
    void *pool=pool_push();
    /* Custom XNU's kern.osversion is set by userspace and is not proof of
     * the mounted iOS build at this lifecycle boundary. Read the real root's
     * build property only after lsd reaches its initialized main runloop. */
    Id version=SEND(klass("NSDictionary"),"dictionaryWithContentsOfFile:",
                   str("/System/Library/CoreServices/SystemVersion.plist"));
    Id build=kind(version,"NSDictionary") ? get(version,"ProductBuildVersion") : NULL;
    if(!kind(build,"NSString") || !equal(build,str("20H392"))) {
        const char *actual=kind(build,"NSString") ? (const char *)SEND(build,"UTF8String") : NULL;
        log_message("SweetRepairPosterMetadata: phase=root build preflight ProductBuildVersion=%s; refusing repair\n",
                    actual ? actual : "unavailable");
        goto done;
    }
    log_message("SweetRepairPosterMetadata: phase=root build preflight ProductBuildVersion=20H392\n");
    Id (*fetch)(Id,Id,Id *)=dlsym(RTLD_DEFAULT,"MobileInstallationCopyFrameworkInfoForLaunchServices");
    if(!fetch) {
        void *mi=dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation",RTLD_NOW|RTLD_LOCAL);
        if(mi) fetch=dlsym(mi,"MobileInstallationCopyFrameworkInfoForLaunchServices");
    }
    if(!fetch) { log_message("SweetRepairPosterMetadata: scoped MI API unavailable\n"); goto done; }
    Id selected[2]={NULL,NULL};
    unsigned int active=0; const char *phase="scoped MI fetch";
    /* Fetch and validate BOTH records before the first possible mutation. */
    for(unsigned int n=0;n<2;n++) {
        active=n; phase="scoped MI fetch";
        log_message("SweetRepairPosterMetadata: provider=%u phase=%s\n",n,phase);
        Id error=NULL, records=fetch(url(providers[n].parent),NULL,&error);
        if(error || !kind(records,"NSArray")) { error_message(n,phase,error); goto refused; }
        phase="complete authoritative MI record validation";
        unsigned long count=((unsigned long (*)(Id,Id))message)(records,selector("count"));
        if(count>1024) goto refused;
        for(unsigned long i=0;i<count;i++) {
            Id item=((Id (*)(Id,Id,unsigned long))message)(records,selector("objectAtIndex:"),i);
            if(!kind(item,"NSDictionary")) goto refused;
            if(!equal(get(item,"CFBundleIdentifier"),str(providers[n].identifier))) continue;
            if(selected[n] || !equal(get(item,"Path"),str(providers[n].path)) ||
               !equal(get(item,"ApplicationType"),str("PluginKitPlugin")) ||
               !kind(get(item,"Entitlements"),"NSDictionary") ||
               !equal(get(get(item,"Entitlements"),"com.apple.posterkit.provider"),
                      SEND(klass("NSNumber"),"numberWithBool:",1)) ||
               !equal(get(item,"CodeSigningInfoNotAuthoritative"),
                      SEND(klass("NSNumber"),"numberWithBool:",0))) goto refused;
            /* Original immutable MI record, without field replacement. */
            selected[n]=item;
        }
        if(!selected[n]) goto refused;
    }
    {
        int before[2];
        for(unsigned int n=0;n<2;n++) {
            active=n; phase="valid LS record entitlement preflight";
            before[n]=record_matches(n,get(selected[n],"Entitlements"));
            log_message("SweetRepairPosterMetadata: provider=%u phase=%s result=%d\n",n,phase,before[n]);
            if(before[n]<0) goto refused;
        }
        Id (*access_queue)(void)=(void *)(core+0xf1bc);
        Id queue=access_queue();
        void (*barrier)(Id,void (^)(void))=dlsym(RTLD_DEFAULT,"dispatch_barrier_sync");
        if(!queue || !barrier) goto refused;
        int (*register_info)(Id,Id)=(void *)(core+0x7a0f4);
        for(unsigned int n=0;n<2;n++) {
            if(before[n]==1) continue;
            Id dictionary=SEND(klass("NSDictionary"),"dictionaryWithObject:forKey:",
                               selected[n],str(providers[n].identifier));
            __block int status=-1;
            barrier(queue, ^{
                status=register_info(url(providers[n].path),dictionary);
                /* Match the native rebuild finish's explicit persistence step.
                 * This only arms LS's real save timer; it is not a flush claim. */
                if(!status) ((void (*)(unsigned long))(core+0xb6db8))(1);
            });
            log_message("SweetRepairPosterMetadata: %s native registration OSStatus=%d\n",providers[n].identifier,status);
            if(status || record_matches(n,get(selected[n],"Entitlements"))!=1) {
                log_message("SweetRepairPosterMetadata: provider=%u postverification failed; no further mutation\n",n);
                goto done;
            }
        }
        /* Read fresh LS records after BOTH operations, never synthesize visibility. */
        if(record_matches(0,get(selected[0],"Entitlements"))==1 &&
           record_matches(1,get(selected[1],"Entitlements"))==1)
            log_message("SweetRepairPosterMetadata: both scoped LS records match genuine MI entitlements; provider discovery unverified\n");
        else log_message("SweetRepairPosterMetadata: final LS verification failed\n");
        goto done;
    }
refused:
    log_message("SweetRepairPosterMetadata: provider=%u phase=%s refused; no further metadata mutation\n",active,phase);
done:
    pool_pop(pool);
}

__attribute__((constructor)) static void initialize(void) {
    if(getuid()!=501 || geteuid()!=501) return;
    int fd=open("/private/var/mobile/Library/Logs/SweetRepairPosterMetadata.log",
                O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd>=0) {
        struct stat st;
        if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==501 && st.st_nlink==1 &&
           (st.st_mode&0777)==0600) diagnostic=fdopen(fd,"a");
        if(!diagnostic) close(fd);
    }
    log_message("SweetRepairPosterMetadata: constructor pid=%d uid=%u\n",getpid(),(unsigned)getuid());
    if(!diagnostic) { log_message("SweetRepairPosterMetadata: owned diagnostic file unavailable; refusing repair\n"); return; }
    if(!image("/usr/libexec/lsd",lsd_uuid)) {
        log_message("SweetRepairPosterMetadata: lsd identity guard refused\n"); return;
    }
    core=image("/System/Library/Frameworks/CoreServices.framework/CoreServices",core_uuid);
    if(!core || !preimage(0x7a0f4,324,"2fce9a7d5729476014eae9c410f0b0de037938e68798f7405021410881dda781") ||
       !preimage(0xf1bc,76,"c9521f4ae251a65dcdb2b0472eee1ca98f34259f11f7d4096cfb8ebb85522a52") ||
       !preimage(0xb6db8,8,"54af41cb2c8960357e7e4d398b088ad3cead9bbce401573cea7930a21bcf8b64")) {
        log_message("SweetRepairPosterMetadata: native CoreServices UUID/function preimage guard refused\n"); return;
    }
    klass=dlsym(RTLD_DEFAULT,"objc_getClass"); selector=dlsym(RTLD_DEFAULT,"sel_registerName");
    message=dlsym(RTLD_DEFAULT,"objc_msgSend"); method=dlsym(RTLD_DEFAULT,"class_getInstanceMethod");
    implementation=dlsym(RTLD_DEFAULT,"method_getImplementation"); encoding=dlsym(RTLD_DEFAULT,"method_getTypeEncoding");
    object_class=dlsym(RTLD_DEFAULT,"object_getClass");
    pool_push=dlsym(RTLD_DEFAULT,"objc_autoreleasePoolPush"); pool_pop=dlsym(RTLD_DEFAULT,"objc_autoreleasePoolPop");
    if(!klass || !selector || !message || !method || !implementation || !encoding || !object_class || !pool_push || !pool_pop) {
        log_message("SweetRepairPosterMetadata: Objective-C runtime unavailable\n"); return;
    }
    if(!pinned_method(klass("LSApplicationExtensionRecord"),"initWithURL:requireValid:error:",0x6204c,"@36@0:8@16B24^@28") ||
       !pinned_method(klass("LSBundleRecord"),"entitlements",0x65b8,"@16@0:8")) {
        log_message("SweetRepairPosterMetadata: native LS record method guard refused\n"); return;
    }
    Id main_queue=dlsym(RTLD_DEFAULT,"_dispatch_main_q");
    Id (*queue_create)(const char *,void *)=dlsym(RTLD_DEFAULT,"dispatch_queue_create");
    void (*async)(Id,void (^)(void))=dlsym(RTLD_DEFAULT,"dispatch_async");
    if(!main_queue || !queue_create || !async) {
        log_message("SweetRepairPosterMetadata: dispatch runtime unavailable\n"); return;
    }
    /* Never synchronously enter LS from main or from the access queue. Keep
     * this serial worker alive for the lsd lifetime; perform one bounded pass. */
    Id worker=queue_create("org.heliodarwin.poster-metadata-repair",NULL);
    if(worker) async(main_queue, ^{
        log_message("SweetRepairPosterMetadata: lsd main runloop ready; scheduling one worker pass\n");
        async(worker, ^{ repair(); });
    });
}
