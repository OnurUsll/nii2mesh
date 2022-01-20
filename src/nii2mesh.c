// gcc -O3 -DHAVE_ZLIB nii2mesh.c meshify.c isolevel.c quadric.c base64.c bwlabel.c radixsort.c -o nii2mesh -lz
// clang -O1 -g -fsanitize=address -fno-omit-frame-pointer -DHAVE_ZLIB nii2mesh.c meshify.c isolevel.c quadric.c base64.c bwlabel.c radixsort.c -o nii2mesh -lz

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include "meshify.h"
#include "nifti1.h"
#include "quadric.h"
#include "isolevel.h"
#if defined(_OPENMP)
	#include <omp.h>
#endif
#ifdef HAVE_ZLIB
	#include <zlib.h>
#endif

float * load_nii(const char *fnm, nifti_1_header * hdr) {
	char imgnm[768], hdrnm[768], basenm[768], ext[768] = "";
	strcpy(basenm, fnm);
	strcpy(imgnm, fnm);
	strcpy(hdrnm, fnm);
	strip_ext(basenm); // ~/file.nii -> ~/file
	if (strlen(fnm) > strlen(basenm))
		strcpy(ext, fnm + strlen(basenm));
	if (strstr(ext, ".hdr")) {
		strcpy(imgnm, basenm);
		strcat(imgnm, ".img");
	}
	if (strstr(ext, ".img")) {
		strcpy(hdrnm, basenm);
		strcat(hdrnm, ".hdr");
	}
	if( access( hdrnm, F_OK ) != 0 ) {
		printf("Unable to find a file named %s\n", hdrnm);
		return NULL;
	}
	if( access( imgnm, F_OK ) != 0 ) {
		printf("Unable to find a file named %s\n", imgnm);
		return NULL;
	}
	bool isGz = false;
	#ifdef HAVE_ZLIB
	if (strstr(ext, ".gz")) {
		gzFile fgz = gzopen(hdrnm, "r");
		if (! fgz) {
			printf("gzopen error %s\n", hdrnm);
			return NULL;
		}
		int bytes_read = gzread(fgz, hdr, sizeof(nifti_1_header));
		gzclose(fgz);
		isGz = true;
		if (bytes_read < sizeof(nifti_1_header)) return NULL;
	}
	#endif
	if (!isGz) {
		FILE *fp = fopen(hdrnm,"rb");
		if (fp == NULL)
			return NULL;
		size_t bytes_read = fread(hdr, sizeof(nifti_1_header), 1, fp);
		fclose(fp);
		if (bytes_read < sizeof(nifti_1_header)) return NULL;
	}
	uint16_t sig = 348;
	uint16_t fwd = hdr->sizeof_hdr;
	uint16_t rev = (fwd & 0xff) << 8 | ((fwd & 0xff00) >> 8);
	if (rev == sig) {
		printf("Demo only reads native endian NIfTI (solution: use niimath)\n");
		return NULL;
	}
	if (fwd != sig) {
		printf("Only compiled to read uncompressed NIfTI (solution: 'gzip -d \"%s\"')\n", hdrnm);
		return NULL;
	}
	if ((hdr->datatype != DT_UINT8) && (hdr->datatype != DT_UINT16) && (hdr->datatype != DT_INT16) && (hdr->datatype != DT_FLOAT32)) {
		printf("Demo does not support this data type (solution: use niimath)\n");
		return NULL;
	}
	int nvox = hdr->dim[1]*hdr->dim[2]*hdr->dim[3];
	if (hdr->scl_slope == 0.0) hdr->scl_slope = 1.0;
	float * img32 = (float *) malloc(nvox*sizeof(float));
	int bpp = 2;
	if (hdr->datatype == DT_UINT8)
		bpp = 1;
	if (hdr->datatype == DT_FLOAT32)
		bpp = 4;
	void * imgRaw = (void *) malloc(nvox*bpp);
	#ifdef HAVE_ZLIB
	if (isGz) {
		gzFile fgz = gzopen(hdrnm, "r");
		if (! fgz)
			return NULL;
		if (hdr->vox_offset > 0) { //skip header
			int nskip = round(hdr->vox_offset);
			void * skip = (void *) malloc(nskip);
			int bytes_read = gzread (fgz, skip, nskip);
			free(skip);
			if (bytes_read != nskip)
				return NULL;
		}
		int bytes_read = gzread(fgz, imgRaw, nvox*bpp);
		if (bytes_read != (nvox*bpp))
			return NULL;
		gzclose(fgz);
	} else
	#endif
	{
		FILE *fp = fopen(imgnm,"rb");
		if (fp == NULL)
			return NULL;
		fseek(fp, (int)hdr->vox_offset, SEEK_SET);
		size_t sz = fread(imgRaw, nvox*bpp, 1, fp);
		fclose(fp);
		if (sz < (nvox*bpp)) return NULL;
	}
	if (hdr->datatype == DT_UINT8) {
		uint8_t * img8 = (uint8_t *) imgRaw;
		for (int i = 0; i < nvox; i++)
			img32[i] = (img8[i] * hdr->scl_slope) + hdr->scl_inter;
	} else if (hdr->datatype == DT_UINT16) {
		uint16_t * img16 = (uint16_t *) imgRaw;
		for (int i = 0; i < nvox; i++)
			img32[i] = (img16[i] * hdr->scl_slope) + hdr->scl_inter;
	} else if (hdr->datatype == DT_INT16) {
		int16_t * img16 = (int16_t *) imgRaw;
		for (int i = 0; i < nvox; i++)
			img32[i] = (img16[i] * hdr->scl_slope) + hdr->scl_inter;
	} else {
		float * img32w = (float *) imgRaw;
		for (int i = 0; i < nvox; i++)
			img32[i] = (img32w[i] * hdr->scl_slope) + hdr->scl_inter;
	}
	free(imgRaw);
	return img32;
}

int nii2 (nifti_1_header hdr, float * img, float isolevel, float reduceFraction, int preSmooth, bool onlyLargest, bool fillBubbles, int postSmooth, bool verbose, char * outnm, int quality) {
	vec3d *pts = NULL;
	vec3i *tris = NULL;
	int ntri, npt;
	size_t dim[3] = {hdr.dim[1], hdr.dim[2], hdr.dim[3]};
	if (meshify(img, dim, isolevel, &tris, &pts, &ntri, &npt, preSmooth, onlyLargest, fillBubbles, verbose) != EXIT_SUCCESS)
		return EXIT_FAILURE;
	apply_sform(tris, pts, ntri, npt, hdr.srow_x, hdr.srow_y, hdr.srow_z);
	double startTime = clockMsec();
	if (postSmooth > 0) {
		laplacian_smoothHC(pts, tris, npt, ntri, 0.1, 0.5, postSmooth, true);
		if (verbose)
			printf("post-smooth: %ld ms\n", timediff(startTime, clockMsec()));
		startTime = clockMsec();
	}
	if ((reduceFraction < 1.0) || (quality > 1)) { //lossless for high quality
		double agressiveness = 7.0; //7 = default for Simplify.h
		if (quality == 0) //fast
			agressiveness = 8.0;
		if (quality == 2) //best
			agressiveness = 5.0;
		int startVert = npt;
		int startTri = ntri;
		int target_count = round((float)ntri * reduceFraction);
		quadric_simplify_mesh(&pts, &tris, &npt, &ntri, target_count, agressiveness, verbose, (quality > 1));
		if (verbose)
			printf("simplify vertices %d->%d triangles %d->%d (r = %g): %ld ms\n", startVert, npt, startTri, ntri, (float)ntri / (float) startTri, timediff(startTime, clockMsec()));
		startTime = clockMsec();
	}
	save_mesh(outnm, tris, pts, ntri, npt, (quality > 0));
	if (verbose)
		printf("save to disk: %ld ms\n", timediff(startTime, clockMsec()));
	free(tris);
	free(pts);
	return EXIT_SUCCESS;
}

int main(int argc,char **argv) {
	#define mxStr 1024
	float isolevel = 0.0;
	int isoDarkMediumBright123 = 2;
	float reduceFraction = 0.25;
	int preSmooth = true;
	bool onlyLargest = true;
	bool fillBubbles = false;
	int postSmooth = 0;
	int quality = 1;
	bool verbose = false;
	char atlasFilename[mxStr] = "";
	// Check the command line, minimal is name of input and output files
	if (argc < 3) {
		printf("Converts a NIfTI voxelwise volume to triangulated mesh.\n");
		printf("Usage: %s inputNIfTI [options] outputMesh\n",argv[0]);
		printf("Options\n");
		printf("    -a s    atlas text file (e.g. '-a D99_v2.0_labels_semicolon.txt')\n");
		printf("    -b v    bubble fill (0=bubbles included, 1=bubbles filled, default %d)\n", fillBubbles);
		printf("    -i v    isosurface intensity (d=dark, m=mid, b=bright, number for custom, default medium)\n");
		printf("    -l v    only keep largest cluster (0=all, 1=largest, default %d)\n", onlyLargest);
		printf("    -p v    pre-smoothing (0=skip, 1=smooth, default %d)\n", preSmooth);
		printf("    -r v    reduction factor (default %g)\n", reduceFraction);
		printf("    -q v    quality (0=fast, 1= balanced, 2=best, default %d)\n", quality);
		printf("    -s v    post-smoothing iterations (default %d)\n", postSmooth);
		printf("    -v v    verbose (0=silent, 1=verbose, default %d)\n", verbose);
		printf("mesh extension sets format (.gii, .mz3, .obj, .ply, .pial, .stl, .vtk)\n");
		printf("Example: '%s voxels.nii mesh.obj'\n",argv[0]);
		printf("Example: '%s bet.nii.gz -i 22 myOutput.obj'\n",argv[0]);
		printf("Example: '%s bet.nii.gz -i b bright.obj'\n",argv[0]);
		printf("Example: '%s img.nii -v 1 out.ply'\n",argv[0]);
		printf("Example: '%s img.nii -p 0 -r 1 large.ply'\n",argv[0]);
		printf("Example: '%s img.nii -r 0.1 small.gii'\n",argv[0]);
		exit(-1);
	}
	// Parse options (if any)
	if (argc > 3) {
		for (int i=2;i<(argc-1);i++) {
			if (strcmp(argv[i],"-a") == 0)
				strcpy(atlasFilename, argv[i+1]);
				//isAtlas = atoi(argv[i+1]);
			if (strcmp(argv[i],"-b") == 0)
				fillBubbles = atoi(argv[i+1]);
			if (strcmp(argv[i],"-i") == 0) {
				if (strlen(argv[i+1]) < 1) continue;
				if (toupper(argv[i+1][0]) == 'D')
					isoDarkMediumBright123 = 1;
				else if (toupper(argv[i+1][0]) == 'M')
					isoDarkMediumBright123 = 2;
				else if (toupper(argv[i+1][0]) == 'B')
					isoDarkMediumBright123 = 3;
				else {
					isoDarkMediumBright123 = 0; //custom
					isolevel = atof(argv[i+1]);
				}
			}
			if (strcmp(argv[i],"-l") == 0)
				onlyLargest = atoi(argv[i+1]);
			if (strcmp(argv[i],"-p") == 0)
				preSmooth = atoi(argv[i+1]);
			if (strcmp(argv[i],"-q") == 0)
				quality = atoi(argv[i+1]);
			if (strcmp(argv[i],"-s") == 0)
				postSmooth = atoi(argv[i+1]);
			if (strcmp(argv[i],"-r") == 0)
				reduceFraction = atof(argv[i+1]);
			if (strcmp(argv[i],"-v") == 0)
				verbose = atoi(argv[i+1]);
		}
	}
	nifti_1_header hdr;
	double startTime = clockMsec();
	float * img = load_nii(argv[1], &hdr);
	if (verbose)
		printf("load from disk: %ld ms\n", timediff(startTime, clockMsec()));
	if (img == NULL)
		exit(EXIT_FAILURE);
	int ret = EXIT_SUCCESS;
	int nvox = (hdr.dim[1] * hdr.dim[2] * hdr.dim[3]);
	if (strlen(atlasFilename) > 0) {
		onlyLargest = false;
		float mx = img[0];
		for (int i = 0; i <= nvox; i++)
			mx = fmax(img[i], mx);
		if (mx < 1.0) {
			printf("intensity range not consistent with an indexed atlas %g..%g\n", hdr.cal_min, hdr.cal_max);
			exit(EXIT_FAILURE);
		}
		int nLabel = trunc(mx);
		char basenm[mxStr], ext[mxStr] = "";
		//look for text file, e.g. atlas.nii.gz -> atlas.txt
		#define kLabelStrLen 32
		char atlasLabels[nLabel+1][kLabelStrLen];
		for (int i = 0; i <= nLabel; i++)
			snprintf (atlasLabels[i], kLabelStrLen-1, "%d", i);
		if (strcmp("1", atlasFilename) != 0) {
			FILE *fp = fopen(atlasFilename,"rt");
			if (fp == NULL) {
				printf("Unable to find atlas names '%s'\n", atlasFilename);
			} else {
				char str[mxStr], s[mxStr];
				while(fgets(str, mxStr, fp)) {
					strncpy(s, strtok(str,";"), mxStr);
					int i = atoi(s);
					if ((i < 0) || (i > nLabel)) continue;
					strncpy(s, strtok(NULL,";"), mxStr);
					//strncpy(atlasLabels[i], strtok(NULL,";"), mxStr);
					if (snprintf (atlasLabels[i], kLabelStrLen-1, "%s.k%d", s, i) < 0) exit(EXIT_FAILURE);
				}
				fclose(fp);
			}
		}
		//next, parse name and extension for output files
		strcpy(basenm, argv[argc-1]);
		strip_ext(basenm); // ~/file.nii -> ~/file
		if (strlen(argv[argc-1]) > strlen(basenm))
			strcpy(ext, argv[argc-1] + strlen(basenm));
		#if defined(_OPENMP) //compile with 'OMP=1 make -j'
			int maxNumThreads = omp_get_max_threads();
			printf("Using %d threads\n", maxNumThreads);
			omp_set_num_threads(maxNumThreads);
		#endif
		int partial_OK, nOK;
		#pragma omp parallel private(partial_OK) shared(nOK)
		{
			partial_OK = 0;
			nOK = 0;
			#pragma omp for
			for (int i = 1; i <= nLabel; i++) {
				printf("%d/%d\n", i, nLabel);
				float * imgbinary = (float *) malloc(nvox*sizeof(float));
				int n1 = 0;
				float lo = i - 0.5;
				float hi = i + 0.5;
				for (int j = 0; j < nvox; j++) {
					int n = 0;
					if ((img[j] > lo) && (img[j] < hi))
						n = 1;
					imgbinary[j] = n;
					n1 += n;
				}
				if (n1 == 0) {
					printf("Skipping %d: no voxels with this intensity\n", i);
					continue;
				}
				char outnm[mxStr];
				if (snprintf(outnm,sizeof(outnm),"%s%s%s", basenm, atlasLabels[i], ext) < 0) exit(EXIT_FAILURE);
				int reti = nii2(hdr, imgbinary, 0.5, reduceFraction, preSmooth, onlyLargest, fillBubbles, postSmooth, verbose, outnm, quality);
				if (reti == EXIT_SUCCESS)
					partial_OK ++;
				free(imgbinary);
			} //for nLabel
			#pragma omp critical
			{
				nOK += partial_OK;
			}
		}	
		printf("Converted %d regions of interest\n", nOK);
		if (nOK == 0)
			ret = EXIT_FAILURE;
	} else {
		if (isoDarkMediumBright123 != 0) //user did not provide numeric isosurface brightness
			isolevel = setThreshold(img, nvox, isoDarkMediumBright123);
		ret = nii2(hdr, img, isolevel, reduceFraction, preSmooth, onlyLargest, fillBubbles, postSmooth, verbose, argv[argc-1], quality);
	}
	free(img);
	exit(ret);
}
