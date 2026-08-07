package com.fourdo.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.fourdo.android.DownloadSourceActivity.LolRomGame;
import com.fourdo.android.IgdbService.IgdbGame;

import java.lang.ref.WeakReference;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public class GameDownloadAdapter extends RecyclerView.Adapter<GameDownloadAdapter.GameViewHolder> {
    
    private static final String TAG = "GameDownloadAdapter";
    
    private Context context;
    private List<LolRomGame> games;
    private Map<String, IgdbGame> igdbCache;
    private OnGameClickListener listener;
    private IgdbService igdbService;
    
    public interface OnGameClickListener {
        void onDownloadClick(LolRomGame game, IgdbGame igdbInfo);
    }
    
    public GameDownloadAdapter(Context context, List<LolRomGame> games, IgdbService igdbService) {
        this.context = context;
        this.games = games;
        this.igdbCache = new ConcurrentHashMap<>();
        this.igdbService = igdbService;
    }
    
    public void setOnGameClickListener(OnGameClickListener listener) {
        this.listener = listener;
    }
    
    public void updateGames(List<LolRomGame> newGames) {
        this.games = newGames;
        this.igdbCache.clear();
        notifyDataSetChanged();
    }
    
    @NonNull
    @Override
    public GameViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(context).inflate(R.layout.item_game_download, parent, false);
        return new GameViewHolder(view);
    }
    
    @Override
    public void onBindViewHolder(@NonNull GameViewHolder holder, int position) {
        LolRomGame game = games.get(position);
        IgdbGame igdbInfo = igdbCache.get(game.title.toLowerCase().trim());
        
        holder.bind(game, igdbInfo);
    }
    
    @Override
    public int getItemCount() {
        return games.size();
    }
    
    class GameViewHolder extends RecyclerView.ViewHolder {
        private ImageView coverImage;
        private TextView titleText;
        private TextView summaryText;
        private TextView regionText;
        private TextView releaseText;
        private ProgressBar coverProgress;
        private Button downloadButton;
        
        GameViewHolder(@NonNull View itemView) {
            super(itemView);
            coverImage = itemView.findViewById(R.id.game_cover);
            titleText = itemView.findViewById(R.id.game_title);
            summaryText = itemView.findViewById(R.id.game_summary);
            regionText = itemView.findViewById(R.id.game_region);
            releaseText = itemView.findViewById(R.id.game_release);
            coverProgress = itemView.findViewById(R.id.cover_progress);
            downloadButton = itemView.findViewById(R.id.download_button);
        }
        
        void bind(LolRomGame game, IgdbGame igdbInfo) {
            titleText.setText(game.title);
            
            if (igdbInfo != null) {
                // Show IGDB info
                summaryText.setText(igdbInfo.summary != null ? 
                    (igdbInfo.summary.length() > 100 ? 
                        igdbInfo.summary.substring(0, 100) + "..." : 
                        igdbInfo.summary) : "");
                summaryText.setVisibility(View.VISIBLE);
                
                regionText.setText(igdbInfo.publisher != null ? igdbInfo.publisher : "");
                releaseText.setText(igdbInfo.releaseDate != null ? igdbInfo.releaseDate : "");
                
                // Load cover
                if (igdbInfo.localCoverPath != null && !igdbInfo.localCoverPath.isEmpty()) {
                    coverProgress.setVisibility(View.GONE);
                    coverImage.setImageBitmap(null);
                    new ImageLoaderTask(coverImage, coverProgress).execute(igdbInfo.localCoverPath);
                } else if (igdbInfo.coverUrl != null && !igdbInfo.coverUrl.isEmpty()) {
                    coverProgress.setVisibility(View.VISIBLE);
                    igdbService.loadCover(igdbInfo.coverUrl, igdbInfo.id, (bitmap, localPath) -> {
                        if (bitmap != null) {
                            coverProgress.setVisibility(View.GONE);
                            coverImage.setImageBitmap(bitmap);
                            if (localPath != null) {
                                igdbInfo.localCoverPath = localPath;
                            }
                        } else {
                            coverProgress.setVisibility(View.GONE);
                        }
                    });
                } else {
                    coverProgress.setVisibility(View.GONE);
                }
            } else {
                // No IGDB info, show placeholder
                summaryText.setVisibility(View.GONE);
                regionText.setText(game.region != null ? game.region : "");
                releaseText.setText(game.fileSize != null ? game.fileSize : "");
                coverProgress.setVisibility(View.GONE);
                coverImage.setImageResource(R.drawable.ic_launcher_foreground);
                
                // Fetch IGDB info asynchronously
                fetchIgdbInfo(game);
            }
            
            downloadButton.setOnClickListener(v -> {
                if (listener != null) {
                    listener.onDownloadClick(game, igdbInfo);
                }
            });
            
            itemView.setOnClickListener(v -> {
                if (listener != null) {
                    listener.onDownloadClick(game, igdbInfo);
                }
            });
        }
        
        private void fetchIgdbInfo(LolRomGame game) {
            String key = game.title.toLowerCase().trim();
            if (igdbCache.containsKey(key)) return;
            
            igdbService.lookupGame(game.title, result -> {
                if (result != null) {
                    igdbCache.put(key, result);
                    int pos = getAdapterPosition();
                    if (pos != RecyclerView.NO_POSITION) {
                        notifyItemChanged(pos);
                    }
                }
            });
        }
    }
    
    // Simple async task for loading local images
    private static class ImageLoaderTask extends android.os.AsyncTask<String, Void, Bitmap> {
        private final WeakReference<ImageView> imageViewRef;
        private final WeakReference<ProgressBar> progressRef;
        
        ImageLoaderTask(ImageView imageView, ProgressBar progress) {
            this.imageViewRef = new WeakReference<>(imageView);
            this.progressRef = new WeakReference<>(progress);
        }
        
        @Override
        protected Bitmap doInBackground(String... params) {
            String path = params[0];
            return BitmapFactory.decodeFile(path);
        }
        
        @Override
        protected void onPostExecute(Bitmap bitmap) {
            ImageView iv = imageViewRef.get();
            ProgressBar pb = progressRef.get();
            if (pb != null) pb.setVisibility(View.GONE);
            if (iv != null && bitmap != null) {
                iv.setImageBitmap(bitmap);
            }
        }
    }
    
    private static class BitmapFactory {
        static Bitmap decodeFile(String path) {
            try {
                java.io.File file = new java.io.File(path);
                if (file.exists()) {
                    android.graphics.BitmapFactory.Options options = new android.graphics.BitmapFactory.Options();
                    options.inPreferredConfig = android.graphics.Bitmap.Config.RGB_565;
                    return android.graphics.BitmapFactory.decodeFile(path, options);
                }
            } catch (Exception e) {}
            return null;
        }
    }
}