# Static site for the mod: the info page plus the launcher packages. Built by site/build.sh, which
# stages index.html and downloads/ as the build context.
FROM busybox:1.37 AS files
WORKDIR /html
COPY index.html ./
COPY downloads/ downloads/
RUN cd downloads && sha256sum NightreignInvasions-* > SHA256SUMS

# Unprivileged nginx: runs as a non-root user and listens on 8080.
FROM nginxinc/nginx-unprivileged:1.29-alpine
COPY --from=files /html /usr/share/nginx/html
